from dataclasses import dataclass


@dataclass(frozen=True)
class Target:
    extension: str
    video: tuple[str, ...] = ()
    audio: tuple[str, ...] = ()
    muxer: tuple[str, ...] = ()
    copy_video: str = ""
    copy_audio: str = ""


H264 = ("-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-pix_fmt", "yuv420p",
        "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2")
AAC = ("-c:a", "aac", "-b:a", "192k")
VP9 = ("-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "5", "-row-mt", "1",
       "-crf", "31", "-b:v", "0", "-pix_fmt", "yuv420p")
OPUS = ("-c:a", "libopus", "-b:a", "128k")

VIDEO = (
    Target("mp4", H264, AAC, ("-movflags", "+faststart"), "h264", "aac"),
    Target("mkv", H264, AAC, copy_video="h264", copy_audio="aac"),
    Target("webm", VP9, OPUS, copy_video="vp9", copy_audio="opus"),
    Target("mov", H264, AAC, ("-movflags", "+faststart"), "h264", "aac"),
)
AUDIO = (
    Target("mp3", audio=("-c:a", "libmp3lame", "-q:a", "2"), copy_audio="mp3"),
    Target("wav", audio=("-c:a", "pcm_s16le"), copy_audio="pcm_s16le"),
    Target("flac", audio=("-c:a", "flac"), copy_audio="flac"),
    Target("m4a", audio=AAC, muxer=("-movflags", "+faststart"), copy_audio="aac"),
    Target("ogg", audio=("-c:a", "libvorbis", "-q:a", "5"), copy_audio="vorbis"),
    Target("opus", audio=OPUS, copy_audio="opus"),
    Target("aac", audio=AAC, copy_audio="aac"),
)
IMAGES = (
    Target("png", video=("-c:v", "png")),
    Target("jpg", video=("-c:v", "mjpeg", "-q:v", "2", "-pix_fmt", "yuvj444p")),
    Target("webp", video=("-c:v", "libwebp", "-lossless", "1", "-compression_level", "3")),
    Target("bmp", video=("-c:v", "bmp")),
    Target("tiff", video=("-c:v", "tiff")),
)

VIDEO_INPUTS = ("mp4", "mkv", "webm", "mov", "avi", "m4v", "wmv", "flv", "mpeg", "mpg",
                "ts", "mts", "m2ts", "3gp", "ogv", "vob")
IMAGE_INPUTS = ("png", "jpg", "jpeg", "webp", "bmp", "tiff", "tif", "gif", "avif", "ico", "tga")
AUDIO_INPUTS = ("mp3", "wav", "flac", "m4a", "ogg", "oga", "opus", "aac", "wma", "aiff",
                "aif", "alac", "amr", "ac3", "mka", "ape")
