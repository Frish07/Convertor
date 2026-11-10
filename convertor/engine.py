import json
import math
import os
import subprocess
import tempfile
import time
from collections.abc import Callable
from dataclasses import dataclass
from itertools import count
from pathlib import Path
from threading import Event, Thread

from .ffmpeg import CREATE_NO_WINDOW, Tools, check_cancelled
from .formats import Target


class ConversionError(Exception):
    pass


@dataclass(frozen=True)
class Media:
    duration: float
    video: dict | None
    audio: dict | None


def probe(tools: Tools, source: Path, cancel: Event) -> Media:
    command = [
        tools.ffprobe, "-v", "error", "-protocol_whitelist", "file",
        "-show_entries", "format=duration:stream=index,codec_type,codec_name,pix_fmt,channels"
        ":stream_disposition=attached_pic", "-of", "json", "-i", source,
    ]
    deadline = time.monotonic() + 30
    with subprocess.Popen(
        command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        creationflags=CREATE_NO_WINDOW,
    ) as process:
        try:
            while True:
                check_cancelled(cancel)
                if time.monotonic() >= deadline:
                    raise ConversionError("Превышено время чтения файла.")
                try:
                    stdout, stderr = process.communicate(timeout=0.15)
                    break
                except subprocess.TimeoutExpired:
                    continue
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
        if process.returncode:
            details = stderr.decode("utf-8", errors="replace").strip()[-2000:]
            raise ConversionError(details or "FFprobe не смог прочитать файл.")
    try:
        data = json.loads(stdout)
    except ValueError as exc:
        raise ConversionError("FFprobe вернул нечитаемые сведения о файле.") from exc
    try:
        duration = float(data.get("format", {}).get("duration", 0))
    except (ValueError, TypeError):
        duration = 0
    video = next(
        (stream for stream in data.get("streams", [])
         if stream.get("codec_type") == "video" and not stream.get("disposition", {}).get("attached_pic")),
        None,
    )
    audio = next((s for s in data.get("streams", []) if s.get("codec_type") == "audio"), None)
    return Media(duration if math.isfinite(duration) and duration > 0 else 0, video, audio)


def encoding_options(
    kind: str, target: Target, media: Media, allow_copy: bool,
) -> tuple[list[str], bool]:
    options = []
    copies_video = copies_audio = False
    if kind in ("video", "image"):
        if media.video is None:
            raise ConversionError("В файле нет видеопотока или изображения.")
        options += ["-map", f"0:{media.video['index']}"]
        copies_video = (
            allow_copy and media.video.get("codec_name") == target.copy_video
            and media.video.get("pix_fmt") == "yuv420p"
        )
        options += ["-c:v", "copy"] if copies_video else target.video
    if kind in ("video", "audio") and media.audio is not None:
        options += ["-map", f"0:{media.audio['index']}"]
        needs_downmix = (
            target.extension in ("mp3", "opus", "ogg", "webm")
            and media.audio.get("channels", 0) > 2
        )
        copies_audio = allow_copy and media.audio.get("codec_name") == target.copy_audio
        options += ["-c:a", "copy"] if copies_audio else target.audio
        if needs_downmix and not copies_audio:
            options += ["-ac", "2"]
    elif kind == "audio":
        raise ConversionError("В файле нет аудиодорожки.")
    if kind == "image":
        options += ["-frames:v", "1", "-update", "1"]
    options += target.muxer
    return options, copies_video or copies_audio


def run_encoder(
    command: list, duration: float, cancel: Event, progress: Callable[[float], None],
) -> None:
    with tempfile.TemporaryFile() as errors, subprocess.Popen(
        command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=errors,
        creationflags=CREATE_NO_WINDOW,
    ) as process:
        def read_progress() -> None:
            for line in process.stdout:
                key, _, value = line.rstrip().partition(b"=")
                if key == b"out_time_us" and duration:
                    try:
                        progress(max(0.0, min(int(value) / (duration * 1_000_000), 0.99)))
                    except ValueError:
                        continue

        reader = Thread(target=read_progress)
        reader.start()
        try:
            while process.poll() is None:
                cancel.wait(0.1)
                check_cancelled(cancel)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            reader.join()
        check_cancelled(cancel)
        if process.returncode:
            errors.seek(max(0, errors.seek(0, os.SEEK_END) - 4000))
            details = errors.read().decode("utf-8", errors="replace").strip()
            raise ConversionError(details or f"FFmpeg завершился с кодом {process.returncode}.")


def publish_output(staged: Path, directory: Path, stem: str, cancel: Event) -> Path:
    # Windows rename and POSIX link both fail atomically if the destination already exists.
    publish = os.rename if os.name == "nt" else os.link
    for number in count(1):
        check_cancelled(cancel)
        name = stem if number == 1 else f"{stem} ({number})"
        destination = directory / f"{name}{staged.suffix}"
        try:
            publish(staged, destination)
        except FileExistsError:
            continue
        return destination


def convert(
    tools: Tools, source: Path, directory: Path, kind: str, target: Target,
    cancel: Event, progress: Callable[[float | None], None],
) -> Path:
    check_cancelled(cancel)
    source = source.absolute()
    directory = directory.absolute()
    if not source.is_file():
        raise ConversionError("Исходный файл недоступен.")
    if not directory.is_dir():
        raise ConversionError("Папка для сохранения недоступна.")
    media = probe(tools, source, cancel)
    duration = media.duration if kind != "image" else 0
    with tempfile.TemporaryDirectory(prefix=".convertor-", dir=directory) as temporary:
        staged = Path(temporary) / f"output.{target.extension}"
        command = [
            tools.ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "error", "-xerror",
            "-nostats", "-progress", "pipe:1", "-abort_on", "empty_output",
            "-y", "-protocol_whitelist", "file", "-i", source,
        ]
        options, copies_stream = encoding_options(kind, target, media, allow_copy=True)
        progress(0 if duration else None)
        try:
            run_encoder([*command, *options, staged], duration, cancel, progress)
        except ConversionError:
            if not copies_stream:
                raise
            check_cancelled(cancel)
            progress(0 if duration else None)
            options, _ = encoding_options(kind, target, media, allow_copy=False)
            run_encoder([*command, *options, staged], duration, cancel, progress)
        if not staged.is_file() or staged.stat().st_size == 0:
            raise ConversionError("FFmpeg не записал выходной файл.")
        return publish_output(staged, directory, source.stem, cancel)
