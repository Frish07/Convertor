# Convertor

**English** | [Русский](README.ru.md)

- [Convertor Web](https://frish07.github.io/Convertor/) — a browser converter that processes files locally.
- [Convertor Desktop](https://github.com/Frish07/Convertor/releases/latest) — a Windows x64 application.

## Convertor Web

Drag files onto the appropriate tab, configure the output for each file, and process individual files or the entire queue. Download results individually or as a ZIP archive. Your language (RU/EN) and theme preferences are saved in the browser.

| Tab | Input files | Output and settings |
| --- | --- | --- |
| Images | PNG, JPEG, WebP, BMP | PNG, JPG, WebP, BMP. Resize, preserve aspect ratio, adjust JPEG/WebP quality, and preview files. PNG and WebP preserve transparency. JPG and 24-bit BMP use a white background. |
| Audio | MP3, WAV, OGG/OGA, FLAC, M4A, AAC, Opus, AIFF, and the video formats listed below | MP3, WAV (PCM), OGG (Vorbis), FLAC, M4A (AAC), Opus. Bitrate settings for lossy formats, quality mode for MP3 and OGG, and compression level for FLAC. |
| Video | MP4, WebM, MKV, MOV, M4V, AVI, OGV | MP4 (H.264/AAC), WebM (VP8/Opus), and audio extraction into any audio output format in this table. Keep the original resolution or limit the height to 1080p, 720p, or 480p while preserving the aspect ratio. Compatible streams can be copied without re-encoding. |
| PDF | PDF | Merge in queue order, split into pages, extract pages, delete pages, and reorder pages. |

User files and results stay in the browser tab's memory. The first audio or video operation downloads single-threaded FFmpeg.wasm 0.12.10 (about 32 MB) from jsDelivr. The application verifies the integrity of the JavaScript and WASM files. Once initialized, the engine is reused in the current tab. [Dependency licenses and source code](https://frish07.github.io/Convertor/third-party-licenses.html)

The per-file limit is 256 MiB for audio and video and 100 MiB for images and PDFs. Memory usage depends on resolution, duration, and the browser, so processing can fail even for smaller files. Decoding depends on the codecs inside the container, while built-in previews use the browser's playback capabilities. Encrypted PDFs must be decrypted before processing. Copying PDF pages may discard interactive forms, signatures, and bookmarks.

### Running and building the web version

Use Node.js 24 LTS. After starting Vite, open the address it prints with the `/Convertor/` path.

```sh
cd web
npm ci
npm run dev
```

Run the following commands for type checking, a production build, and browser tests.

```sh
npm run check
npm run build
npx playwright install chromium
npm test
```

The static build is written to `web/dist`. The `Convertor Web` workflow tests it in Chromium and deploys it to GitHub Pages when the web files change on `main`. For initial setup, select **GitHub Actions** under **Settings → Pages → Build and deployment → Source**, then run the workflow. [Configuring a GitHub Pages publishing source](https://docs.github.com/en/pages/getting-started-with-github-pages/configuring-a-publishing-source-for-your-github-pages-site)

## Convertor Desktop

A desktop video, image, and audio converter for Windows 10 (22H2) and Windows 11 on x64. The interface and processing management are written in C++20 using the Win32 API. FFmpeg runs in a separate process, and compatible streams are copied without re-encoding.

## Getting started

1. Download the Windows x64 archive from [Releases](https://github.com/Frish07/Convertor/releases/latest).
2. Extract the entire archive and run `Convertor.exe`.
3. If prompted to download FFmpeg, click **Скачать FFmpeg** (Download FFmpeg). The download (~110 MB) is required once. The application verifies the archive's SHA-256 checksum and saves the engine in your local Windows profile. File conversion then works offline.
4. Select a tab and add files using the file picker or by dragging them into the window. Choose the output format for the entire queue in the top card or for an individual file in its row. Click **Конвертировать** (Convert).

By default, results are saved alongside the source files. You can choose an output folder on each tab. If a filename already exists, Convertor adds a number, such as `clip (2).mp4`. Click a filename in the queue to open its result or view error details. Cancelling stops the current operation and keeps the results of files that have already finished.

## Output formats

| Tab | Output formats | Examples |
| --- | --- | --- |
| Video | MP4, MKV, WebM, MOV, and all audio formats | MKV → MP4, MP4 → MP3 |
| Images | PNG, JPG, WebP, BMP, TIFF | WebP → PNG |
| Audio | MP3, WAV, FLAC, M4A, OGG, Opus, AAC | OGG → MP3 |

Video conversion uses the first video stream and the first audio track. Audio extraction keeps the first audio track. The Images tab writes a single frame.

Convertor uses `ffmpeg` and `ffprobe` from PATH, the application folder, or its `ffmpeg/bin` subfolder. The built-in download button retrieves [FFmpeg Essentials for Windows](https://github.com/GyanD/codexffmpeg/releases/tag/9.0.2) by Gyan Doshi (GPLv3). [FFmpeg source code for this build](https://github.com/FFmpeg/FFmpeg/commit/946fcce07b)

## Building and testing

Extract [LLVM-MinGW for Windows x64 with UCRT](https://github.com/mstorsjo/llvm-mingw/releases/tag/20260908). Pass the path to the toolchain folder to the build script.

```powershell
.\scripts\build_windows.ps1 -Toolchain 'C:\dev\llvm-mingw-20260908-ucrt-x86_64'
```

The executable is written to `build/v2`, and the release archive is written to `dist`.

For integration tests, provide the folder containing `ffmpeg.exe` and `ffprobe.exe` from FFmpeg Essentials.

```powershell
.\tests\check_windows.ps1 -Toolchain 'C:\dev\llvm-mingw-20260908-ucrt-x86_64' -FFmpegDirectory 'C:\dev\ffmpeg\bin'
```

The tests generate media files and check all output formats, audio extraction, transparency, preservation of the source file, and cancellation. The `-CheckDownload` flag also tests the built-in FFmpeg download in a separate test folder.
