#include "media.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace media;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string bytes(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

void verify(const Tools& tools, const fs::path& samples, const fs::path& outputs) {
    const auto video = samples / L"clip.mkv";
    const auto image = samples / L"image source.webp";
    const auto audio = samples / L"audio sample.ogg";
    auto originalAudio = bytes(audio);
    const Progress ignore = [](auto) {};
    for (const auto& format : formats()) {
        const auto& source = format.kind == Kind::video ? video : format.kind == Kind::image ? image : audio;
        auto output = convert(tools, source, outputs, format, {}, ignore);
        require(output == outputs / (source.stem().native() + L"." + format.extension), "Unexpected name");
        require(fs::file_size(output) > 0, "Empty output");
    }
    const Format* mp3 = nullptr;
    const Format* webm = nullptr;
    for (const auto& format : formats()) {
        if (format.extension == L"mp3") mp3 = &format;
        if (format.extension == L"webm") webm = &format;
    }
    require(mp3 && webm, "Missing conversion presets");
    auto first = outputs / L"audio sample.mp3";
    auto originalResult = bytes(first);
    auto second = convert(tools, audio, outputs, *mp3, {}, ignore);
    require(second == outputs / L"audio sample (2).mp3", "Collision was not renamed");
    require(bytes(first) == originalResult, "Existing output was changed");
    require(bytes(audio) == originalAudio, "Input was changed");
    require(convert(tools, video, outputs, *mp3, {}, ignore) == outputs / L"clip.mp3", "No extracted track");

    for (const auto& sample : {L"silent.mkv", L"damaged.mkv"}) {
        try {
            convert(tools, samples / sample, outputs, *mp3, {}, ignore);
            throw std::runtime_error("Invalid input succeeded");
        } catch (const Failure&) {}
    }
    std::stop_source stop;
    try {
        convert(tools, video, outputs, *webm, stop.get_token(), [&](auto value) {
            if (value && *value > 0) stop.request_stop();
        });
        throw std::runtime_error("Cancelled conversion succeeded");
    } catch (const Cancelled&) {}
    for (const auto& entry : fs::directory_iterator(outputs)) {
        require(entry.is_regular_file(), "A temporary directory was left behind");
    }
    require(!fs::exists(outputs / L"silent.mp3") && !fs::exists(outputs / L"damaged.mp3") &&
            !fs::exists(outputs / L"clip (2).webm"), "An unsuccessful conversion published an output");
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--install") {
            auto tools = media::installTools({}, [](auto) {});
            require(media::findTools({}).has_value() && media::fs::is_regular_file(tools.ffprobe),
                    "Downloaded FFmpeg was not detected");
            std::cout << "FFmpeg download, SHA-256, extraction and detection passed\n";
            return 0;
        }
        require(argc == 3, "Usage: MediaTests.exe samples outputs");
        auto tools = media::findTools({});
        require(tools.has_value(), "FFmpeg and FFprobe were not detected");
        verify(*tools, media::fs::absolute(argv[1]), media::fs::absolute(argv[2]));
        std::cout << "16 output formats, extraction, collisions, invalid inputs and cancellation passed\n";
        return 0;
    } catch (const media::Failure& error) {
        std::wcerr << error.message << L'\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
