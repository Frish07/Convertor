import subprocess
import tempfile
import unittest
from pathlib import Path
from threading import Event

from convertor.engine import ConversionError, convert, encoding_options, probe
from convertor.ffmpeg import Cancelled, find_tools
from convertor.formats import AUDIO, IMAGES, VIDEO


class MediaConversionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tools = find_tools()
        if cls.tools is None:
            raise RuntimeError("Install FFmpeg and FFprobe before running the conversion tests.")

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name) / "Проба файлов"
        self.directory.mkdir()
        self.cancel = Event()

    def ffmpeg(self, *arguments):
        subprocess.run([self.tools.ffmpeg, "-hide_banner", "-loglevel", "error", "-xerror",
                        "-nostdin", *arguments], check=True, timeout=30)

    def make_video(self, with_audio=True):
        video = self.directory / "ролик.mkv"
        self.ffmpeg(
            "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=24",
            *(["-f", "lavfi", "-i", "sine=frequency=440", "-c:a", "aac"] if with_audio else []),
            "-t", "0.7", "-c:v", "libx264", "-pix_fmt", "yuv420p", video,
        )
        return video

    def check_output(self, source, kind, target, video_codec=None, audio_codec=None):
        output = convert(self.tools, source, self.directory, kind, target, self.cancel, lambda _: None)
        self.assertEqual(output.suffix, "." + target.extension)
        self.ffmpeg("-i", output, "-f", "null", "-")
        info = probe(self.tools, output, self.cancel)
        self.assertEqual(info.video and info.video["codec_name"], video_codec)
        self.assertEqual(info.audio and info.audio["codec_name"], audio_codec)
        return output, info

    def test_all_audio_targets_and_filename_collision(self):
        source = self.directory / "запись.ogg"
        self.ffmpeg("-f", "lavfi", "-i", "sine=frequency=880", "-t", "0.5", "-c:a", "libvorbis", source)
        original = source.read_bytes()
        for target, codec in zip(AUDIO, ("mp3", "pcm_s16le", "flac", "aac", "vorbis", "opus", "aac"),
                                 strict=True):
            with self.subTest(target=target.extension):
                output, _ = self.check_output(source, "audio", target, audio_codec=codec)
                if output.suffix == source.suffix:
                    self.assertEqual(output.name, "запись (2).ogg")
        first_mp3 = self.directory / "запись.mp3"
        first_bytes = first_mp3.read_bytes()
        second, _ = self.check_output(source, "audio", AUDIO[0], audio_codec="mp3")
        self.assertEqual(second.name, "запись (2).mp3")
        self.assertEqual(first_mp3.read_bytes(), first_bytes)
        self.assertEqual(source.read_bytes(), original)

    def test_all_video_targets_and_audio_extraction(self):
        source = self.make_video()
        source_media = probe(self.tools, source, self.cancel)
        options, copied = encoding_options("video", VIDEO[0], source_media, allow_copy=True)
        self.assertTrue(copied)
        self.assertEqual(options.count("copy"), 2)
        for target, codecs in zip(VIDEO, (("h264", "aac"), ("h264", "aac"), ("vp9", "opus"),
                                          ("h264", "aac")), strict=True):
            with self.subTest(target=target.extension):
                self.check_output(source, "video", target, *codecs)
        self.check_output(source, "audio", AUDIO[0], audio_codec="mp3")

    def test_all_image_targets_and_webp_transparency(self):
        source = self.directory / "картинка с пробелом.webp"
        self.ffmpeg("-f", "lavfi", "-i", "color=c=red@0.5:s=66x54,format=rgba", "-frames:v", "1",
                    "-c:v", "libwebp", "-lossless", "1", source)
        for target, codec in zip(IMAGES, ("png", "mjpeg", "webp", "bmp", "tiff"), strict=True):
            with self.subTest(target=target.extension):
                _, info = self.check_output(source, "image", target, video_codec=codec)
                if codec == "png":
                    self.assertEqual(info.video["pix_fmt"], "rgba")

    def test_missing_audio_and_corrupted_input_leave_no_output(self):
        source = self.make_video(with_audio=False)
        with self.assertRaisesRegex(ConversionError, "нет аудиодорожки"):
            convert(self.tools, source, self.directory, "audio", AUDIO[0], self.cancel, lambda _: None)
        corrupt = self.directory / "не читается.mkv"
        corrupt.write_bytes(b"invalid media")
        with self.assertRaises(ConversionError):
            convert(self.tools, corrupt, self.directory, "video", VIDEO[0], self.cancel, lambda _: None)
        self.assertCountEqual(self.directory.iterdir(), (source, corrupt))

    def test_cancelled_conversion_does_not_publish_output(self):
        source = self.make_video()
        with self.assertRaises(Cancelled):
            convert(self.tools, source, self.directory, "video", VIDEO[2], self.cancel,
                    lambda _: self.cancel.set())
        self.assertEqual(list(self.directory.iterdir()), [source])


if __name__ == "__main__":
    unittest.main()
