#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace media {
namespace fs = std::filesystem;

enum class Kind { video, image, audio };

struct Format {
    std::wstring extension;
    Kind kind;
    std::vector<std::wstring> video;
    std::vector<std::wstring> audio;
    std::vector<std::wstring> container;
    std::string copyVideo;
    std::string copyAudio;
};

struct Tools {
    fs::path ffmpeg, ffprobe;
};

struct Failure {
    std::wstring message;
};

struct Cancelled {};

using Progress = std::function<void(std::optional<double>)>;

const std::vector<Format>& formats();
const std::vector<std::wstring>& inputExtensions(Kind kind);
std::wstring extensionOf(const fs::path& path);
std::wstring fromUtf8(const std::string& text);
std::optional<Tools> findTools(std::stop_token stop);
Tools installTools(std::stop_token stop, const Progress& progress);
fs::path convert(const Tools& tools, const fs::path& source, const fs::path& directory,
                 const Format& format, std::stop_token stop, const Progress& progress);
}
