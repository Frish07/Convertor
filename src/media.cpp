#include "media.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string_view>

namespace media {
namespace {
using namespace std::chrono_literals;
using Args = std::vector<std::wstring>;

const Args h264{L"-c:v", L"libx264", L"-preset", L"veryfast", L"-crf", L"20",
                L"-pix_fmt", L"yuv420p", L"-vf", L"pad=ceil(iw/2)*2:ceil(ih/2)*2"};
const Args aac{L"-c:a", L"aac", L"-b:a", L"192k"};
const Args vp9{L"-c:v", L"libvpx-vp9", L"-deadline", L"good", L"-cpu-used", L"5",
               L"-row-mt", L"1", L"-crf", L"31", L"-b:v", L"0", L"-pix_fmt", L"yuv420p"};
const Args opus{L"-c:a", L"libopus", L"-b:a", L"128k"};
const Args fastStart{L"-movflags", L"+faststart"};

constexpr auto buildName = L"ffmpeg-9.0.2-essentials_build";
constexpr auto downloadPath =
    L"/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-essentials_build.zip";
constexpr std::size_t archiveSize = 114768076;
constexpr std::string_view archiveSha256 =
    "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba";

void cancelled(std::stop_token stop) {
    if (stop.stop_requested()) throw Cancelled{};
}

[[noreturn]] void winFailure(const std::wstring& operation, DWORD code = GetLastError()) {
    wchar_t* message = nullptr;
    DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                               FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                               reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring detail = size ? std::wstring(message, size) : std::to_wstring(code);
    if (message) LocalFree(message);
    while (!detail.empty() && iswspace(detail.back())) detail.pop_back();
    throw Failure{operation + L"\n" + detail};
}

struct CloseKernel {
    void operator()(void* handle) const { CloseHandle(handle); }
};
struct CloseHttp {
    void operator()(void* handle) const { WinHttpCloseHandle(handle); }
};
struct CloseAlgorithm {
    void operator()(void* handle) const { BCryptCloseAlgorithmProvider(handle, 0); }
};
struct CloseHash {
    void operator()(void* handle) const { BCryptDestroyHash(handle); }
};
using Handle = std::unique_ptr<void, CloseKernel>;
using Http = std::unique_ptr<void, CloseHttp>;

std::wstring environment(const wchar_t* name) {
    DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (!length) return {};
    std::wstring value(length, L'\0');
    DWORD written = GetEnvironmentVariableW(name, value.data(), length);
    if (written >= length) return {};
    value.resize(written);
    return value;
}

fs::path appDirectory() {
    std::wstring path(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) winFailure(L"Не удалось определить папку приложения.");
    path.resize(length);
    return fs::path(path).parent_path();
}

fs::path cacheDirectory() {
    auto local = environment(L"LOCALAPPDATA");
    if (local.empty()) throw Failure{L"Папка LOCALAPPDATA недоступна."};
    return fs::path(local) / L"Convertor" / L"ffmpeg-9.0.2";
}

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const fs::path& parent) {
        GUID id{};
        if (FAILED(CoCreateGuid(&id))) throw Failure{L"Не удалось создать временную папку."};
        wchar_t name[40]{};
        StringFromGUID2(id, name, 40);
        path = parent / (std::wstring(L".convertor-") + name);
        if (!fs::create_directory(path)) throw Failure{L"Не удалось создать временную папку."};
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    fs::path path;
};

std::wstring quote(const std::wstring& arg) {
    std::wstring result(1, L'"');
    unsigned slashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += ch;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

struct ProcessResult {
    DWORD code;
    std::string output;
};

ProcessResult run(const fs::path& executable, const Args& arguments, std::stop_token stop,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds::max(),
                  const std::function<void(std::string_view)>& onLine = {}) {
    cancelled(stop);
    std::wstring command = quote(executable.native());
    for (const auto& arg : arguments) {
        command += L" ";
        command += quote(arg);
    }

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE readHandle{}, writeHandle{};
    if (!CreatePipe(&readHandle, &writeHandle, &inherit, 0)) winFailure(L"Не удалось открыть канал FFmpeg.");
    Handle readPipe(readHandle), writePipe(writeHandle);
    if (!SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0)) {
        winFailure(L"Не удалось настроить канал FFmpeg.");
    }
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &inherit, OPEN_EXISTING, 0, nullptr);
    if (nullInput == INVALID_HANDLE_VALUE) winFailure(L"Не удалось открыть входной канал.");
    Handle input(nullInput);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.get();
    startup.hStdOutput = startup.hStdError = writePipe.get();
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) {
        winFailure(L"Не удалось запустить " + executable.filename().native() + L".");
    }
    Handle process(info.hProcess), thread(info.hThread);
    writePipe.reset();

    std::string output, pending;
    const auto start = std::chrono::steady_clock::now();
    auto drain = [&] {
        std::array<char, 8192> buffer{};
        DWORD available = 0;
        while (PeekNamedPipe(readPipe.get(), nullptr, 0, nullptr, &available, nullptr) && available) {
            cancelled(stop);
            DWORD count = 0;
            if (!ReadFile(readPipe.get(), buffer.data(), std::min<DWORD>(available, buffer.size()),
                          &count, nullptr) || !count) break;
            output.append(buffer.data(), count);
            if (output.size() > 131072) output.erase(0, output.size() - 131072);
            if (onLine) {
                pending.append(buffer.data(), count);
                std::size_t startLine = 0;
                for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n', startLine)) {
                    std::string_view line(pending.data() + startLine, end - startLine);
                    if (line.ends_with('\r')) line.remove_suffix(1);
                    onLine(line);
                    startLine = end + 1;
                }
                pending.erase(0, startLine);
                if (pending.size() > 65536) pending.clear();
            }
        }
    };
    try {
        while (true) {
            cancelled(stop);
            drain();
            DWORD waited = WaitForSingleObject(process.get(), 30);
            if (waited == WAIT_OBJECT_0) break;
            if (waited == WAIT_FAILED) winFailure(L"Не удалось дождаться завершения FFmpeg.");
            if (timeout != std::chrono::milliseconds::max() &&
                std::chrono::steady_clock::now() - start > timeout) {
                throw Failure{L"Превышено время ожидания ответа от " + executable.filename().native() + L"."};
            }
        }
        drain();
        cancelled(stop);
    } catch (...) {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), INFINITE);
        throw;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(process.get(), &code)) winFailure(L"Не удалось прочитать код завершения.");
    return {code, std::move(output)};
}

Tools toolsIn(const fs::path& path) {
    return {path / L"ffmpeg.exe", path / L"ffprobe.exe"};
}

bool valid(const Tools& tools, std::stop_token stop) {
    std::error_code error;
    if (!fs::is_regular_file(tools.ffmpeg, error) || !fs::is_regular_file(tools.ffprobe, error)) return false;
    try {
        auto probe = run(tools.ffprobe, {L"-hide_banner", L"-version"}, stop, 5s);
        if (probe.code || !probe.output.starts_with("ffprobe version ")) return false;
        auto encoders = run(tools.ffmpeg, {L"-hide_banner", L"-encoders"}, stop, 5s);
        if (encoders.code) return false;
        for (auto codec : {"libx264", "libvpx-vp9", "libmp3lame", "libvorbis", "libopus", "libwebp"}) {
            if (encoders.output.find(codec) == std::string::npos) return false;
        }
        return true;
    } catch (const Failure&) {
        return false;
    }
}

struct Stream {
    int index = -1;
    int channels = 0;
    bool cover = false;
    std::string kind, codec, pixelFormat;
};

struct Description {
    std::optional<Stream> video, audio;
    double seconds = 0;
};

[[noreturn]] void processFailure(const ProcessResult& result) {
    auto output = result.output.size() > 3200 ? result.output.substr(result.output.size() - 3200) : result.output;
    throw Failure{output.empty() ? L"FFmpeg завершился с кодом " + std::to_wstring(result.code)
                                 : fromUtf8(output)};
}

Description inspect(const Tools& tools, const fs::path& source, std::stop_token stop) {
    auto result = run(tools.ffprobe, {L"-v", L"error", L"-protocol_whitelist", L"file",
                      L"-show_entries", L"format=duration:stream=index,codec_type,codec_name,pix_fmt,channels"
                      L":stream_disposition=attached_pic", L"-of", L"flat", L"-i", source.native()}, stop, 30s);
    if (result.code) processFailure(result);
    std::istringstream lines(result.output);
    std::map<int, Stream> streams;
    Description description;
    std::string line;
    while (std::getline(lines, line)) {
        if (line.ends_with('\r')) line.pop_back();
        auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string_view key(line.data(), equals), value(line.data() + equals + 1, line.size() - equals - 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value.remove_prefix(1);
            value.remove_suffix(1);
        }
        if (key == "format.duration") {
            double seconds = 0;
            std::from_chars(value.data(), value.data() + value.size(), seconds);
            if (std::isfinite(seconds) && seconds > 0) description.seconds = seconds;
            continue;
        }
        constexpr std::string_view prefix = "streams.stream.";
        if (!key.starts_with(prefix)) continue;
        key.remove_prefix(prefix.size());
        auto dot = key.find('.');
        if (dot == std::string_view::npos) continue;
        int slot = 0;
        if (std::from_chars(key.data(), key.data() + dot, slot).ec != std::errc{}) continue;
        key.remove_prefix(dot + 1);
        auto& stream = streams[slot];
        if (key == "index") std::from_chars(value.data(), value.data() + value.size(), stream.index);
        else if (key == "channels") std::from_chars(value.data(), value.data() + value.size(), stream.channels);
        else if (key == "codec_name") stream.codec = value;
        else if (key == "codec_type") stream.kind = value;
        else if (key == "pix_fmt") stream.pixelFormat = value;
        else if (key == "disposition.attached_pic") stream.cover = value == "1";
    }
    for (const auto& [slot, stream] : streams) {
        if (stream.index < 0) continue;
        if (!description.video && stream.kind == "video" && !stream.cover) description.video = stream;
        if (!description.audio && stream.kind == "audio") description.audio = stream;
    }
    return description;
}

bool encodingOptions(Args& command, const Format& format, const Description& input, bool allowCopy) {
    bool copied = false;
    auto append = [&](const Args& options) { command.insert(command.end(), options.begin(), options.end()); };
    if (format.kind != Kind::audio) {
        if (!input.video) throw Failure{L"В файле нет видеопотока или изображения."};
        append({L"-map", L"0:" + std::to_wstring(input.video->index)});
        bool copy = allowCopy && !format.copyVideo.empty() && input.video->codec == format.copyVideo
                    && input.video->pixelFormat == "yuv420p";
        append(copy ? Args{L"-c:v", L"copy"} : format.video);
        copied |= copy;
    }
    if (format.kind != Kind::image && input.audio) {
        append({L"-map", L"0:" + std::to_wstring(input.audio->index)});
        bool copy = allowCopy && !format.copyAudio.empty() && input.audio->codec == format.copyAudio;
        append(copy ? Args{L"-c:a", L"copy"} : format.audio);
        const auto& ext = format.extension;
        if (!copy && input.audio->channels > 2 && (ext == L"mp3" || ext == L"ogg" || ext == L"opus" || ext == L"webm")) {
            append({L"-ac", L"2"});
        }
        copied |= copy;
    } else if (format.kind == Kind::audio) {
        throw Failure{L"В файле нет аудиодорожки."};
    }
    if (format.kind == Kind::image) append({L"-frames:v", L"1", L"-update", L"1"});
    append(format.container);
    return copied;
}

void download(const fs::path& path, std::stop_token stop, const Progress& progress) {
    cancelled(stop);
    Http session(WinHttpOpen(L"Convertor/2.0.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) winFailure(L"Не удалось открыть соединение с GitHub.");
    WinHttpSetTimeouts(session.get(), 15000, 15000, 15000, 15000);
    Http connection(WinHttpConnect(session.get(), L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) winFailure(L"Не удалось подключиться к GitHub.");
    Http request(WinHttpOpenRequest(connection.get(), L"GET", downloadPath, nullptr,
                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request || !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                 || !WinHttpReceiveResponse(request.get(), nullptr)) {
        winFailure(L"Не удалось загрузить FFmpeg с GitHub.");
    }
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             nullptr, &status, &size, nullptr)) {
        winFailure(L"Не удалось прочитать ответ GitHub.");
    }
    if (status != 200) throw Failure{L"GitHub вернул HTTP " + std::to_wstring(status) + L"."};

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw Failure{L"Не удалось запустить проверку SHA-256."};
    }
    std::unique_ptr<void, CloseAlgorithm> algorithmGuard(algorithm);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        throw Failure{L"Не удалось создать SHA-256."};
    }
    std::unique_ptr<void, CloseHash> hashGuard(hash);
    std::ofstream output(path, std::ios::binary);
    if (!output) throw Failure{L"Не удалось записать архив FFmpeg."};
    std::array<unsigned char, 64 * 1024> chunk{};
    std::size_t received = 0, reported = 0;
    progress(0);
    while (true) {
        cancelled(stop);
        DWORD count = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &count)) {
            winFailure(L"Загрузка FFmpeg прервалась.");
        }
        if (!count) break;
        received += count;
        if (received > archiveSize || BCryptHashData(hash, chunk.data(), count, 0) < 0) {
            throw Failure{L"Не удалось проверить загруженный архив FFmpeg."};
        }
        output.write(reinterpret_cast<const char*>(chunk.data()), count);
        if (!output) throw Failure{L"Не удалось записать архив FFmpeg."};
        if (received - reported >= archiveSize / 100) {
            reported = received;
            progress(static_cast<double>(received) / archiveSize);
        }
    }
    output.close();
    std::array<unsigned char, 32> digest{};
    if (!output || received != archiveSize || BCryptFinishHash(hash, digest.data(), digest.size(), 0) < 0) {
        throw Failure{L"Архив FFmpeg загрузился не полностью."};
    }
    std::string hex;
    constexpr char digits[] = "0123456789abcdef";
    for (auto byte : digest) {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    if (hex != archiveSha256) throw Failure{L"Контрольная сумма архива FFmpeg не совпала."};
    progress(std::nullopt);
}
}

const std::vector<Format>& formats() {
    static const std::vector<Format> targets{
        {L"mp4", Kind::video, h264, aac, fastStart, "h264", "aac"},
        {L"mkv", Kind::video, h264, aac, {}, "h264", "aac"},
        {L"webm", Kind::video, vp9, opus, {}, "vp9", "opus"},
        {L"mov", Kind::video, h264, aac, fastStart, "h264", "aac"},
        {L"png", Kind::image, {L"-c:v", L"png"}, {}, {}, {}, {}},
        {L"jpg", Kind::image, {L"-c:v", L"mjpeg", L"-q:v", L"2", L"-pix_fmt", L"yuvj444p"}, {}, {}, {}, {}},
        {L"webp", Kind::image, {L"-c:v", L"libwebp", L"-lossless", L"1", L"-compression_level", L"3"}, {}, {}, {}, {}},
        {L"bmp", Kind::image, {L"-c:v", L"bmp"}, {}, {}, {}, {}},
        {L"tiff", Kind::image, {L"-c:v", L"tiff"}, {}, {}, {}, {}},
        {L"mp3", Kind::audio, {}, {L"-c:a", L"libmp3lame", L"-q:a", L"2"}, {}, {}, "mp3"},
        {L"wav", Kind::audio, {}, {L"-c:a", L"pcm_s16le"}, {}, {}, "pcm_s16le"},
        {L"flac", Kind::audio, {}, {L"-c:a", L"flac"}, {}, {}, "flac"},
        {L"m4a", Kind::audio, {}, aac, fastStart, {}, "aac"},
        {L"ogg", Kind::audio, {}, {L"-c:a", L"libvorbis", L"-q:a", L"5"}, {}, {}, "vorbis"},
        {L"opus", Kind::audio, {}, opus, {}, {}, "opus"},
        {L"aac", Kind::audio, {}, aac, {}, {}, "aac"},
    };
    return targets;
}

const std::vector<std::wstring>& inputExtensions(Kind kind) {
    static const std::vector<std::wstring> video{L"mp4", L"mkv", L"webm", L"mov", L"avi", L"m4v", L"wmv",
        L"flv", L"mpeg", L"mpg", L"ts", L"mts", L"m2ts", L"3gp", L"ogv", L"vob"};
    static const std::vector<std::wstring> image{L"png", L"jpg", L"jpeg", L"webp", L"bmp", L"tiff", L"tif",
        L"gif", L"avif", L"ico", L"tga"};
    static const std::vector<std::wstring> audio{L"mp3", L"wav", L"flac", L"m4a", L"ogg", L"oga", L"opus",
        L"aac", L"wma", L"aiff", L"aif", L"alac", L"amr", L"ac3", L"mka", L"ape"};
    if (kind == Kind::video) return video;
    if (kind == Kind::image) return image;
    return audio;
}

std::wstring extensionOf(const fs::path& path) {
    auto extension = path.extension().native();
    if (!extension.empty()) extension.erase(0, 1);
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    return extension;
}

std::wstring fromUtf8(const std::string& text) {
    if (text.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::optional<Tools> findTools(std::stop_token stop) {
    auto app = appDirectory();
    std::vector<fs::path> directories{app, app / L"ffmpeg" / L"bin"};
    if (!environment(L"LOCALAPPDATA").empty()) directories.insert(directories.begin(), cacheDirectory());
    std::wistringstream path(environment(L"PATH"));
    for (std::wstring part; std::getline(path, part, L';');) {
        if (part.size() >= 2 && part.front() == L'"' && part.back() == L'"') part = part.substr(1, part.size() - 2);
        if (fs::path(part).is_absolute()) directories.emplace_back(part);
    }
    for (const auto& directory : directories) {
        cancelled(stop);
        auto tools = toolsIn(directory);
        if (valid(tools, stop)) return tools;
    }
    return std::nullopt;
}

Tools installTools(std::stop_token stop, const Progress& progress) {
    auto destination = cacheDirectory();
    auto tools = toolsIn(destination);
    if (valid(tools, stop)) return tools;
    fs::create_directories(destination.parent_path());
    TemporaryDirectory temporary(destination.parent_path());
    auto archive = temporary.path / L"ffmpeg.zip";
    download(archive, stop, progress);
    auto bin = temporary.path / L"bin";
    fs::create_directory(bin);
    wchar_t systemPath[MAX_PATH]{};
    if (!GetSystemDirectoryW(systemPath, MAX_PATH)) winFailure(L"Системная папка Windows недоступна.");
    auto base = std::wstring(buildName) + L"/bin/";
    auto unpack = run(fs::path(systemPath) / L"tar.exe", {L"-xf", archive.native(), L"-C", bin.native(),
                      L"--strip-components", L"2", base + L"ffmpeg.exe", base + L"ffprobe.exe"}, stop, 120s);
    if (unpack.code) processFailure(unpack);
    cancelled(stop);
    if (!MoveFileW(bin.c_str(), destination.c_str())) {
        DWORD moveError = GetLastError();
        if (valid(tools, stop)) return tools;
        if (!fs::is_directory(destination)) winFailure(L"Не удалось сохранить FFmpeg.", moveError);
        for (const auto& executable : {L"ffmpeg.exe", L"ffprobe.exe"}) {
            if (!MoveFileExW((bin / executable).c_str(), (destination / executable).c_str(), MOVEFILE_REPLACE_EXISTING)) {
                DWORD replaceError = GetLastError();
                if (valid(tools, stop)) return tools;
                winFailure(L"Не удалось заменить FFmpeg.", replaceError);
            }
        }
    }
    if (!valid(tools, stop)) throw Failure{L"Загруженная сборка FFmpeg не запускается."};
    return tools;
}

fs::path convert(const Tools& tools, const fs::path& source, const fs::path& directory,
                 const Format& format, std::stop_token stop, const Progress& progress) {
    cancelled(stop);
    if (!fs::is_regular_file(source)) throw Failure{L"Исходный файл недоступен."};
    if (!fs::is_directory(directory)) throw Failure{L"Папка для сохранения недоступна."};
    auto input = inspect(tools, fs::absolute(source), stop);
    double seconds = format.kind == Kind::image ? 0 : input.seconds;
    TemporaryDirectory temporary(fs::absolute(directory));
    auto staged = temporary.path / (L"output." + format.extension);
    const Args base{L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-xerror", L"-nostats",
                    L"-progress", L"pipe:1", L"-abort_on", L"empty_output", L"-y", L"-protocol_whitelist",
                    L"file", L"-i", fs::absolute(source).native()};
    auto encode = [&](Args command) {
        progress(seconds ? std::optional<double>(0) : std::nullopt);
        command.push_back(staged.native());
        return run(tools.ffmpeg, command, stop, std::chrono::milliseconds::max(), [&](std::string_view line) {
            constexpr std::string_view prefix = "out_time_us=";
            if (seconds && line.starts_with(prefix)) {
                line.remove_prefix(prefix.size());
                long long time = 0;
                if (std::from_chars(line.data(), line.data() + line.size(), time).ec == std::errc{}) {
                    progress(std::clamp(time / (seconds * 1000000), 0.0, 0.99));
                }
            }
        });
    };
    auto command = base;
    bool copied = encodingOptions(command, format, input, true);
    auto result = encode(std::move(command));
    if (result.code && copied) {
        command = base;
        encodingOptions(command, format, input, false);
        result = encode(std::move(command));
    }
    if (result.code) processFailure(result);
    if (!fs::is_regular_file(staged) || !fs::file_size(staged)) throw Failure{L"FFmpeg не записал результат."};
    auto stem = source.stem().native();
    for (unsigned number = 1;; ++number) {
        cancelled(stop);
        auto suffix = number == 1 ? std::wstring{} : L" (" + std::to_wstring(number) + L")";
        auto output = fs::absolute(directory) / (stem + suffix + L"." + format.extension);
        if (MoveFileW(staged.c_str(), output.c_str())) return output;
        DWORD code = GetLastError();
        std::error_code error;
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS && !fs::exists(output, error)) {
            winFailure(L"Не удалось сохранить результат.", code);
        }
    }
}
}
