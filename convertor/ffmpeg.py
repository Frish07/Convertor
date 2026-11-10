import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from threading import Event
from urllib.request import urlopen


CREATE_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
DOWNLOAD_VERSION = "9.0.2"
BUILD_NAME = f"ffmpeg-{DOWNLOAD_VERSION}-essentials_build"
DOWNLOAD_URL = (
    f"https://github.com/GyanD/codexffmpeg/releases/download/{DOWNLOAD_VERSION}/{BUILD_NAME}.zip"
)
DOWNLOAD_HASH = "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba"
DOWNLOAD_SIZE = 114768076
REQUIRED_ENCODERS = {
    b"libx264", b"libvpx-vp9", b"libmp3lame", b"libvorbis", b"libopus", b"libwebp",
}


class Cancelled(Exception):
    pass


def check_cancelled(cancel: Event) -> None:
    if cancel.is_set():
        raise Cancelled


@dataclass(frozen=True)
class Tools:
    ffmpeg: Path
    ffprobe: Path


def tools_in(directory: Path) -> Tools:
    suffix = ".exe" if sys.platform == "win32" else ""
    return Tools(directory / f"ffmpeg{suffix}", directory / f"ffprobe{suffix}")


def managed_directory() -> Path:
    local_data = Path(os.environ.get("LOCALAPPDATA") or Path.home() / "AppData" / "Local")
    return local_data / "Convertor" / f"ffmpeg-{DOWNLOAD_VERSION}"


def validate(tools: Tools) -> bool:
    for name, executable in (("ffmpeg", tools.ffmpeg), ("ffprobe", tools.ffprobe)):
        try:
            if not executable.is_file():
                return False
            result = subprocess.run(
                [executable, "-hide_banner", "-version"], stdin=subprocess.DEVNULL, capture_output=True,
                timeout=5, creationflags=CREATE_NO_WINDOW,
            )
        except (OSError, subprocess.TimeoutExpired):
            return False
        if result.returncode or not result.stdout.startswith(f"{name} version ".encode()):
            return False
    try:
        result = subprocess.run(
            [tools.ffmpeg, "-hide_banner", "-encoders"], stdin=subprocess.DEVNULL, capture_output=True,
            timeout=5, creationflags=CREATE_NO_WINDOW,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    available = {fields[1] for line in result.stdout.splitlines() if len(fields := line.split()) >= 2}
    return not result.returncode and REQUIRED_ENCODERS <= available


def find_tools() -> Tools | None:
    app_dir = (
        Path(sys.executable).parent if getattr(sys, "frozen", False)
        else Path(__file__).resolve().parent.parent
    )
    candidates = [tools_in(app_dir), tools_in(app_dir / "ffmpeg" / "bin")]
    if sys.platform == "win32":
        candidates.insert(0, tools_in(managed_directory()))
    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    if ffmpeg and ffprobe:
        candidates.append(Tools(Path(ffmpeg).resolve(), Path(ffprobe).resolve()))
    return next((candidate for candidate in candidates if validate(candidate)), None)


def install_tools(cancel: Event, progress: Callable[[float | None], None]) -> Tools:
    destination = managed_directory()
    if validate(tools_in(destination)):
        return tools_in(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".download-", dir=destination.parent) as directory:
        archive_path = Path(directory) / "ffmpeg.zip"
        digest = hashlib.sha256()
        received = 0
        progress(0)
        with urlopen(DOWNLOAD_URL, timeout=20) as response, archive_path.open("wb") as output:
            while chunk := response.read(256 * 1024):
                check_cancelled(cancel)
                received += len(chunk)
                if received > DOWNLOAD_SIZE:
                    raise ValueError("Размер архива FFmpeg превышает ожидаемый.")
                digest.update(chunk)
                output.write(chunk)
                progress(received / DOWNLOAD_SIZE)
        check_cancelled(cancel)
        if digest.hexdigest() != DOWNLOAD_HASH:
            raise ValueError("Контрольная сумма архива FFmpeg не совпала.")
        progress(None)
        stage = Path(directory) / "bin"
        stage.mkdir()
        with zipfile.ZipFile(archive_path) as archive:
            for name in ("ffmpeg.exe", "ffprobe.exe"):
                with archive.open(f"{BUILD_NAME}/bin/{name}") as source:
                    with (stage / name).open("wb") as output:
                        while chunk := source.read(1024 * 1024):
                            check_cancelled(cancel)
                            output.write(chunk)
        check_cancelled(cancel)
        try:
            stage.rename(destination)
        except OSError:
            if not destination.is_dir():
                raise
            if not validate(tools_in(destination)):
                for executable in stage.iterdir():
                    executable.replace(destination / executable.name)
        if not validate(tools_in(destination)):
            raise ValueError("Загруженный FFmpeg не запускается на этом компьютере.")
    return tools_in(destination)
