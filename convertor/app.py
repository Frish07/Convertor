import os
import sys
import tkinter as tk
import zipfile
from pathlib import Path
from queue import Empty, SimpleQueue
from threading import Event, Thread
from tkinter import filedialog, messagebox, ttk

from . import VERSION
from .engine import ConversionError, convert
from .ffmpeg import Cancelled, find_tools, install_tools
from .formats import AUDIO, AUDIO_INPUTS, IMAGES, IMAGE_INPUTS, VIDEO, VIDEO_INPUTS


BACKGROUND = "#f1f5fb"
SURFACE = "#ffffff"
INK = "#172640"
MUTED = "#4c607d"
ACCENT = "#2556d8"
NAVY = "#12213d"
BORDER = "#cdd8e8"
FONT = "Segoe UI"


def configure_style(root):
    style = ttk.Style(root)
    style.theme_use("clam")
    style.configure(".", font=(FONT, 10), foreground=INK, background=SURFACE)
    style.configure("Body.TFrame", background=BACKGROUND)
    style.configure("Body.TLabel", background=BACKGROUND, foreground=MUTED)
    style.configure("Hint.TLabel", foreground=MUTED)
    style.configure("Strong.TLabel", font=(FONT, 10, "bold"))
    style.configure("TButton", padding=(11, 6), background=SURFACE, bordercolor=BORDER)
    style.map("TButton", background=[("pressed", "#dde8fb"), ("active", "#eaf1ff")])
    style.configure("Primary.TButton", background=ACCENT, foreground=SURFACE, bordercolor=ACCENT,
                    font=(FONT, 10, "bold"), padding=(15, 7))
    style.map("Primary.TButton",
              background=[("disabled", "#dfe6f2"), ("pressed", "#173da7"), ("active", "#1b47bf")],
              foreground=[("disabled", MUTED)])
    style.configure("TNotebook", background=BACKGROUND, bordercolor=BORDER, tabmargins=(0, 0, 0, 0))
    style.configure("TNotebook.Tab", padding=(19, 8), background="#e3ebf8", foreground=MUTED)
    style.map("TNotebook.Tab", background=[("selected", SURFACE)], foreground=[("selected", INK)])
    style.configure("Treeview", rowheight=30, fieldbackground=SURFACE, bordercolor=BORDER)
    style.configure("Treeview.Heading", padding=(8, 5), font=(FONT, 9, "bold"), background="#eaf0fa")
    style.map("Treeview", background=[("selected", "#dfe9ff")], foreground=[("selected", INK)])
    style.configure("TCombobox", padding=4)
    style.configure("Horizontal.TProgressbar", background=ACCENT, troughcolor="#dce5f3",
                    bordercolor="#dce5f3", lightcolor=ACCENT, darkcolor=ACCENT)


class Pane(ttk.Frame):
    def __init__(self, parent, app, kind, hint, inputs, targets):
        super().__init__(parent, padding=(16, 14))
        self.app = app
        self.kind = kind
        self.inputs = inputs
        self.targets = targets
        self.directory = None
        self.paths = {}
        self.results = {}
        self.errors = {}
        self.format = tk.StringVar(value=targets[0].extension.upper())
        self.folder_label = tk.StringVar(value="Рядом с исходными файлами")
        self.count_label = tk.StringVar(value="Файлов в списке: 0")
        self.columnconfigure(0, weight=1)
        self.rowconfigure(2, weight=1)

        ttk.Label(self, text=hint, style="Hint.TLabel", wraplength=600).grid(row=0, column=0, sticky="w")
        toolbar = ttk.Frame(self)
        toolbar.grid(row=1, column=0, sticky="ew", pady=(10, 7))
        ttk.Label(toolbar, textvariable=self.count_label, style="Strong.TLabel").pack(side="left")
        self.remove_button = ttk.Button(toolbar, text="Убрать выбранные", command=self.remove_files)
        self.remove_button.pack(side="right")
        self.add_button = ttk.Button(toolbar, text="+  Добавить файлы", command=self.add_files)
        self.add_button.pack(side="right", padx=(0, 8))

        table = ttk.Frame(self)
        table.grid(row=2, column=0, sticky="nsew")
        self.tree = ttk.Treeview(table, columns=("status",), show="tree headings", height=7)
        self.tree.heading("#0", text="Исходный файл", anchor="w")
        self.tree.heading("status", text="Состояние", anchor="w")
        self.tree.column("#0", width=340, minwidth=180)
        self.tree.column("status", width=140, minwidth=125, stretch=False)
        scrollbar = ttk.Scrollbar(table, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side="right", fill="y")
        self.tree.pack(side="left", fill="both", expand=True)
        self.tree.bind("<<TreeviewSelect>>", lambda _event: self.refresh())
        self.tree.bind("<Double-1>", self.show_details)
        self.tree.tag_configure("alternate", background="#f6f8fc")

        output = ttk.Frame(self)
        output.grid(row=3, column=0, sticky="ew", pady=(13, 0))
        output.columnconfigure(1, weight=1)
        ttk.Label(output, text="Формат результата", style="Strong.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(output, text="Папка для сохранения", style="Strong.TLabel").grid(
            row=0, column=1, columnspan=3, sticky="w", padx=(16, 0),
        )
        self.format_box = ttk.Combobox(output, textvariable=self.format, width=16, state="readonly",
                                      values=[target.extension.upper() for target in targets])
        self.format_box.grid(row=1, column=0, sticky="ew", pady=(4, 0))
        ttk.Label(output, textvariable=self.folder_label, width=1, style="Hint.TLabel").grid(
            row=1, column=1, sticky="ew", padx=(16, 8), pady=(4, 0),
        )
        self.folder_button = ttk.Button(output, text="Обзор…", command=self.choose_folder)
        self.folder_button.grid(row=1, column=2, pady=(4, 0))
        self.reset_button = ttk.Button(output, text="Сбросить", command=self.reset_folder)
        self.reset_button.grid(row=1, column=3, padx=(6, 0), pady=(4, 0))

        actions = ttk.Frame(self)
        actions.grid(row=4, column=0, sticky="ew", pady=(11, 0))
        ttk.Label(actions, text="Двойной щелчок открывает результат или детали ошибки.",
                  style="Hint.TLabel").pack(side="left")
        self.start_button = ttk.Button(actions, text="Конвертировать", style="Primary.TButton",
                                       command=lambda: app.start_batch(self))
        self.start_button.pack(side="right")

    def refresh(self):
        self.count_label.set(f"Файлов в списке: {len(self.paths)}")
        idle = not self.app.busy
        self.add_button.state(["!disabled" if idle else "disabled"])
        self.folder_button.state(["!disabled" if idle else "disabled"])
        self.remove_button.state(["!disabled" if idle and self.tree.selection() else "disabled"])
        self.reset_button.state(["!disabled" if idle and self.directory else "disabled"])
        self.start_button.state(["!disabled" if idle and self.paths and self.app.tools else "disabled"])
        self.format_box.configure(state="readonly" if idle else "disabled")

    def add_files(self):
        selected = filedialog.askopenfilenames(
            parent=self, title="Добавить файлы",
            filetypes=[("Поддерживаемые файлы", tuple(f"*.{ext}" for ext in self.inputs)),
                       ("Все файлы", "*")],
        )
        ignored = []
        for name in selected:
            path = Path(name)
            if path.suffix.lower().lstrip(".") not in self.inputs:
                ignored.append(path.name)
                continue
            if path in self.paths.values():
                continue
            tag = "alternate" if len(self.paths) % 2 else ""
            row = self.tree.insert("", "end", text=path.name, values=("В списке",), tags=(tag,))
            self.paths[row] = path
        self.refresh()
        if ignored:
            messagebox.showinfo("Формат файла", "Для этой вкладки пропущены файлы:\n" + "\n".join(ignored),
                                parent=self)

    def remove_files(self):
        for row in self.tree.selection():
            self.tree.delete(row)
            self.paths.pop(row, None)
            self.results.pop(row, None)
            self.errors.pop(row, None)
        for index, row in enumerate(self.paths):
            self.tree.item(row, tags=("alternate",) if index % 2 else ())
        self.refresh()

    def choose_folder(self):
        initial = self.directory or (next(iter(self.paths.values())).parent if self.paths else Path.home())
        selected = filedialog.askdirectory(parent=self, initialdir=initial, mustexist=True)
        if selected:
            self.directory = Path(selected)
            self.folder_label.set(selected)
            self.refresh()

    def reset_folder(self):
        self.directory = None
        self.folder_label.set("Рядом с исходными файлами")
        self.refresh()

    def show_details(self, event):
        row = self.tree.identify_row(event.y)
        if row in self.errors:
            messagebox.showerror(self.paths[row].name, self.errors[row], parent=self)
        elif row in self.results:
            result = self.results[row]
            try:
                os.startfile(result)
            except (AttributeError, OSError):
                messagebox.showinfo("Файл сохранён", str(result), parent=self)
        elif row in self.paths:
            messagebox.showinfo("Исходный файл", str(self.paths[row]), parent=self)


class App:
    def __init__(self, root):
        self.root = root
        self.tools = None
        self.busy = True
        self.checking = True
        self.closing = False
        self.pane = None
        self.events = SimpleQueue()
        self.cancel = Event()
        self.indeterminate = False
        self.engine_label = tk.StringVar(value="Проверка FFmpeg…")
        self.status = tk.StringVar(value="Подготовка к работе")

        root.title(f"Convertor  ·  {VERSION}")
        root.geometry("790x620")
        root.minsize(720, 560)
        root.configure(background=BACKGROUND)
        root.protocol("WM_DELETE_WINDOW", self.close)
        configure_style(root)

        header = tk.Frame(root, background=NAVY, padx=22, pady=14)
        header.pack(fill="x")
        tk.Label(header, text="Convertor", background=NAVY, foreground=SURFACE,
                 font=(FONT, 24, "bold")).grid(row=0, column=0, sticky="w")
        tk.Label(header, text="Видео, изображения и аудио", background=NAVY, foreground="#c4d4ee",
                 font=(FONT, 10)).grid(row=1, column=0, sticky="w")
        header.columnconfigure(1, weight=1)
        tk.Label(header, text=f"v{VERSION}", background="#223859", foreground="#e0ecff",
                 font=(FONT, 10, "bold"), padx=9, pady=4).grid(row=0, column=1, sticky="e")

        body = ttk.Frame(root, style="Body.TFrame", padding=(20, 8, 20, 12))
        body.pack(fill="both", expand=True)
        engine_bar = ttk.Frame(body, style="Body.TFrame")
        engine_bar.pack(fill="x", pady=(0, 8))
        ttk.Label(engine_bar, textvariable=self.engine_label, style="Body.TLabel").pack(side="left")
        self.install_button = ttk.Button(engine_bar, text="Скачать FFmpeg", command=self.install)
        self.install_button.pack(side="right")

        notebook = ttk.Notebook(body)
        tabs = (
            ("Видео", "video", "Конвертируйте видео или извлекайте из него аудиодорожку.",
             VIDEO_INPUTS, VIDEO + AUDIO),
            ("Изображения", "image", "Меняйте формат изображений. Результат сохраняется одним кадром.",
             IMAGE_INPUTS, IMAGES),
            ("Аудио", "audio", "Меняйте формат музыки, записей и других аудиофайлов.",
             AUDIO_INPUTS, AUDIO),
        )
        self.panes = []
        for label, kind, hint, inputs, targets in tabs:
            pane = Pane(notebook, self, kind, hint, inputs, targets)
            notebook.add(pane, text=label)
            self.panes.append(pane)

        footer = ttk.Frame(body, style="Body.TFrame")
        footer.pack(side="bottom", fill="x", pady=(10, 0))
        notebook.pack(fill="both", expand=True)
        footer.columnconfigure(0, weight=1)
        ttk.Label(footer, textvariable=self.status, style="Body.TLabel", width=1).grid(
            row=0, column=0, sticky="ew",
        )
        self.bar = ttk.Progressbar(footer, mode="determinate", maximum=100)
        self.bar.grid(row=1, column=0, sticky="ew", pady=(4, 0))
        self.cancel_button = ttk.Button(footer, text="Отмена", command=self.cancel_operation)
        self.cancel_button.grid(row=0, column=1, rowspan=2, padx=(12, 0))
        self.refresh()
        Thread(target=lambda: self.events.put(("tools", find_tools(), None)), daemon=True).start()
        self.root.after(60, self.poll)

    def refresh(self):
        for pane in self.panes:
            pane.refresh()
        can_install = sys.platform == "win32" and not self.busy and self.tools is None
        self.install_button.state(["!disabled" if can_install else "disabled"])
        self.cancel_button.state(["!disabled" if self.busy and self.cancelable else "disabled"])

    @property
    def cancelable(self):
        return self.busy and not self.checking and not self.cancel.is_set()

    def set_progress(self, value):
        if value is None:
            if not self.indeterminate:
                self.bar.configure(mode="indeterminate")
                self.bar.start(14)
                self.indeterminate = True
        else:
            if self.indeterminate:
                self.bar.stop()
                self.bar.configure(mode="determinate")
                self.indeterminate = False
            self.bar.configure(value=value * 100)

    def install(self):
        self.busy = True
        self.cancel.clear()
        self.engine_label.set("FFmpeg Essentials · Gyan.dev")
        self.status.set("Загрузка FFmpeg (~110 МБ)…")
        self.set_progress(None)
        self.refresh()

        def work():
            try:
                tools = install_tools(self.cancel, lambda value: self.events.put(("progress", value)))
                self.events.put(("tools", tools, None))
            except Cancelled:
                self.events.put(("tools", None, None))
            except (OSError, ValueError, KeyError, zipfile.BadZipFile) as exc:
                self.events.put(("tools", None, str(exc)))

        Thread(target=work, daemon=True).start()

    def start_batch(self, pane):
        self.busy = True
        self.pane = pane
        self.cancel.clear()
        pane.results.clear()
        pane.errors.clear()
        paths = tuple(pane.paths.items())
        target = next(target for target in pane.targets if target.extension.upper() == pane.format.get())
        directory = pane.directory
        kind = "audio" if target in AUDIO else pane.kind
        tools = self.tools
        for row, _ in paths:
            pane.tree.set(row, "status", "В очереди")
        self.status.set("Чтение файлов…")
        self.set_progress(None)
        self.refresh()

        def work():
            completed = failed = 0
            for index, (row, path) in enumerate(paths, start=1):
                if self.cancel.is_set():
                    break
                self.events.put(("begin", row, f"{index}/{len(paths)}  ·  {path.name}"))
                try:
                    result = convert(
                        tools, path, directory or path.parent, kind, target, self.cancel,
                        lambda value: self.events.put(("progress", value)),
                    )
                except Cancelled:
                    self.events.put(("cancelled", row))
                    break
                except (ConversionError, OSError) as exc:
                    failed += 1
                    self.events.put(("error", row, str(exc)))
                else:
                    completed += 1
                    self.events.put(("result", row, result))
            self.events.put(("done", completed, failed, len(paths) - completed - failed))

        Thread(target=work, daemon=True).start()

    def poll(self):
        try:
            while True:
                self.handle_event(*self.events.get_nowait())
        except Empty:
            pass
        if self.closing and not self.busy:
            self.root.destroy()
        else:
            self.root.after(60, self.poll)

    def handle_event(self, event, *data):
        if event == "progress":
            if not self.cancel.is_set():
                self.set_progress(data[0])
        elif event == "tools":
            self.checking = False
            self.tools, error = data
            self.engine_label.set("FFmpeg готов к работе" if self.tools else "Для конвертации требуется FFmpeg")
            self.status.set("Добавьте файлы и выберите формат." if self.tools else "Укажите FFmpeg в PATH или скачайте его.")
            self.finish()
            if error and not self.closing:
                messagebox.showerror("Не удалось скачать FFmpeg", error, parent=self.root)
        elif event == "done":
            completed, failed, skipped = data
            if skipped:
                for row in self.pane.paths:
                    if self.pane.tree.set(row, "status") == "В очереди":
                        self.pane.tree.set(row, "status", "Пропущен")
            parts = [f"Готово: {completed}", f"Ошибок: {failed}"]
            if skipped:
                parts.append(f"Прервано: {skipped}")
            self.status.set("  ·  ".join(parts))
            self.finish(0 if skipped else 1)
        else:
            row = data[0]
            if event == "begin":
                self.pane.tree.set(row, "status", "Обработка…")
                if not self.cancel.is_set():
                    self.status.set(data[1])
                    self.set_progress(None)
            elif event == "result":
                self.pane.results[row] = data[1]
                self.pane.tree.set(row, "status", "Готово")
            elif event == "error":
                self.pane.errors[row] = data[1]
                self.pane.tree.set(row, "status", "Ошибка")
            elif event == "cancelled":
                self.pane.tree.set(row, "status", "Отменён")

    def finish(self, progress=0):
        self.busy = False
        self.pane = None
        self.set_progress(progress)
        self.refresh()

    def cancel_operation(self):
        self.cancel.set()
        self.status.set("Отмена операции…")
        self.refresh()

    def close(self):
        if self.busy:
            if self.cancelable and not messagebox.askyesno(
                "Выход", "Остановить текущую операцию и закрыть Convertor?", parent=self.root,
            ):
                return
            self.closing = True
            self.cancel_operation()
        else:
            self.root.destroy()


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()
