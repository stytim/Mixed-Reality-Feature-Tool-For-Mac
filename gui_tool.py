import sys
import pathlib
import customtkinter as ctk
from tkinter import filedialog
import threading
import queue

import core_logic as core

# --- Application Configuration ---
ctk.set_appearance_mode("dark")
ctk.set_default_color_theme("blue")

class App(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.title("MRTK Feature Tool")
        self.geometry("700x600")
        
        self.project_path: pathlib.Path | None = None
        self.mrtk_components: dict = {}
        self.mrtk_releases_json: list = []
        self.user_selections: dict = {}
        self.openxr_selections: set = set()
        self.resolved_packages: dict = {}

        self.grid_rowconfigure(0, weight=1)
        self.grid_columnconfigure(0, weight=1)
        self.current_frame = None
        self.show_project_select_frame()

    def show_frame(self, frame_class, **kwargs):
        if self.current_frame:
            self.current_frame.destroy()
        self.current_frame = frame_class(self, **kwargs)
        self.current_frame.grid(row=0, column=0, padx=10, pady=10, sticky="nsew")

    def show_project_select_frame(self):
        self.show_frame(ProjectSelectFrame)

    def show_feature_select_frame(self):
        self.show_frame(FeatureSelectFrame)

    def show_confirmation_frame(self):
        self.show_frame(ConfirmationFrame)
    
    # --- FIX IS HERE ---
    # The method now accepts the 'for_import' argument and passes it on.
    def show_progress_frame(self, for_import=False):
        self.show_frame(ProgressFrame, for_import=for_import)

    def show_completion_frame(self):
        self.show_frame(CompletionFrame)

# --- UI Frames ---

class ProjectSelectFrame(ctk.CTkFrame):
    def __init__(self, master, **kwargs):
        super().__init__(master, **kwargs)
        self.app = master
        self.grid_columnconfigure(1, weight=1)
        ctk.CTkLabel(self, text="Select project", font=ctk.CTkFont(size=24, weight="bold")).grid(row=0, column=0, columnspan=3, padx=20, pady=(20, 10), sticky="w")
        self.path_entry = ctk.CTkEntry(self, placeholder_text="/Users/username/UnityProject")
        self.path_entry.grid(row=2, column=1, padx=5, pady=20, sticky="ew")
        ctk.CTkButton(self, text="...", width=40, command=self.browse_for_project).grid(row=2, column=2, padx=(5, 20), pady=20)
        self.version_label_value = ctk.CTkLabel(self, text="<None>", anchor="w")
        self.version_label_value.grid(row=3, column=1, padx=5, pady=5, sticky="w")
        ctk.CTkLabel(self, text="Project Path:").grid(row=2, column=0, padx=(20, 5), pady=20, sticky="w")
        ctk.CTkLabel(self, text="Project Unity version:").grid(row=3, column=0, padx=(20, 5), pady=5, sticky="w")
        self.discover_button = ctk.CTkButton(self, text="Discover Features", command=self.discover_features_clicked, state="disabled")
        self.discover_button.grid(row=5, column=2, padx=20, pady=20, sticky="e")

    def browse_for_project(self):
        path_str = filedialog.askdirectory(title="Select a Unity Project Folder")
        if path_str:
            self.path_entry.delete(0, "end")
            self.path_entry.insert(0, path_str)
            self.validate_project_path()

    def validate_project_path(self):
        path = pathlib.Path(self.path_entry.get())
        unity_version = core.get_unity_version(path)
        if unity_version:
            self.app.project_path = path
            self.version_label_value.configure(text=unity_version)
            self.discover_button.configure(state="normal")
        else:
            self.app.project_path = None
            self.version_label_value.configure(text="<Invalid Project>")
            self.discover_button.configure(state="disabled")

    def discover_features_clicked(self):
        self.discover_button.configure(text="Discovering...", state="disabled")
        threading.Thread(target=self.run_discovery_worker, daemon=True).start()

    def run_discovery_worker(self):
        components, releases_json = core.discover_mrtk_components()
        self.app.mrtk_components = components
        self.app.mrtk_releases_json = releases_json
        self.app.after(0, self.app.show_feature_select_frame)

class FeatureSelectFrame(ctk.CTkFrame):
    def __init__(self, master, **kwargs):
        super().__init__(master, **kwargs)
        self.app = master
        self.selection_vars = {}
        self.grid_columnconfigure(0, weight=1)
        self.grid_rowconfigure(1, weight=1)
        ctk.CTkLabel(self, text="Discover Features", font=ctk.CTkFont(size=24, weight="bold")).grid(row=0, column=0, padx=20, pady=(20, 10), sticky="w")
        self.scroll_frame = ctk.CTkScrollableFrame(self)
        self.scroll_frame.grid(row=1, column=0, padx=20, pady=10, sticky="nsew")
        self.scroll_frame.grid_columnconfigure(1, weight=1)
        row = 0
        ctk.CTkLabel(self.scroll_frame, text="Mixed Reality Toolkit", font=ctk.CTkFont(size=16, weight="bold")).grid(row=row, padx=5, pady=5, sticky="w"); row += 1
        for name in sorted(self.app.mrtk_components.keys()):
            versions = self.app.mrtk_components[name]
            cb_var = ctk.StringVar(value="off")
            ctk.CTkCheckBox(self.scroll_frame, text=name, variable=cb_var, onvalue=name, offvalue="off").grid(row=row, column=0, padx=10, pady=5, sticky="w")
            opt_menu = ctk.CTkOptionMenu(self.scroll_frame, values=versions)
            opt_menu.grid(row=row, column=2, padx=10, pady=5, sticky="e")
            self.selection_vars[name] = {"type": "mrtk", "cb_var": cb_var, "opt_menu": opt_menu}; row += 1
        ctk.CTkLabel(self.scroll_frame, text="OpenXR Components", font=ctk.CTkFont(size=16, weight="bold")).grid(row=row, padx=5, pady=(20, 5), sticky="w"); row += 1
        openxr_packages = {"Microsoft Mixed Reality OpenXR": "com.microsoft.mixedreality.openxr", "Meta OpenXR": "com.unity.xr.meta-openxr"}
        for display_name, identifier in openxr_packages.items():
            cb_var = ctk.StringVar(value="off")
            ctk.CTkCheckBox(self.scroll_frame, text=display_name, variable=cb_var, onvalue=identifier, offvalue="off").grid(row=row, column=0, columnspan=3, padx=10, pady=5, sticky="w")
            self.selection_vars[identifier] = {"type": "openxr", "cb_var": cb_var}; row += 1
        ctk.CTkButton(self, text="Get Features", command=self.get_features_clicked).grid(row=2, column=0, padx=20, pady=20, sticky="e")

    def get_features_clicked(self):
        self.app.user_selections = {}
        self.app.openxr_selections = set()
        for identifier, data in self.selection_vars.items():
            if data["cb_var"].get() != "off":
                if data["type"] == "mrtk": self.app.user_selections[identifier] = data["opt_menu"].get()
                elif data["type"] == "openxr": self.app.openxr_selections.add(identifier)
        if not self.app.user_selections and not self.app.openxr_selections: return
        self.app.show_progress_frame(for_import=False)

class ConfirmationFrame(ctk.CTkFrame):
    def __init__(self, master, **kwargs):
        super().__init__(master, **kwargs)
        self.app = master
        self.grid_columnconfigure((0, 1), weight=1); self.grid_rowconfigure(1, weight=1)
        ctk.CTkLabel(self, text="Import Features", font=ctk.CTkFont(size=24, weight="bold")).grid(row=0, column=0, columnspan=2, padx=20, pady=(20, 10), sticky="w")
        features_frame = ctk.CTkScrollableFrame(self, label_text="Features"); features_frame.grid(row=1, column=0, padx=(20,10), pady=10, sticky="nsew")
        for name, version in self.app.user_selections.items():
            ctk.CTkLabel(features_frame, text=f"✔️ {name} {version}").pack(padx=10, pady=2, anchor="w")
        deps_frame = ctk.CTkScrollableFrame(self, label_text="Required Dependencies"); deps_frame.grid(row=1, column=1, padx=(10,20), pady=10, sticky="nsew")
        for name, version in self.app.resolved_packages.items():
            if name not in self.app.user_selections:
                 ctk.CTkLabel(deps_frame, text=f"✔️ {name} {version}").pack(padx=10, pady=2, anchor="w")
        ctk.CTkButton(self, text="Import", command=self.import_clicked).grid(row=2, column=1, padx=20, pady=20, sticky="e")

    def import_clicked(self):
        self.app.show_progress_frame(for_import=True)

class ProgressFrame(ctk.CTkFrame):
    def __init__(self, master, for_import=False, **kwargs):
        super().__init__(master, **kwargs)
        self.app = master
        self.grid_columnconfigure(0, weight=1); self.grid_rowconfigure(1, weight=1)
        title = "Importing Features" if for_import else "Resolving Dependencies"
        ctk.CTkLabel(self, text=title, font=ctk.CTkFont(size=24, weight="bold")).grid(row=0, column=0, padx=20, pady=20, sticky="w")
        self.log_box = ctk.CTkTextbox(self, state="disabled", font=("Courier New", 12)); self.log_box.grid(row=1, column=0, padx=20, pady=10, sticky="nsew")
        self.progress_bar = ctk.CTkProgressBar(self); self.progress_bar.grid(row=2, column=0, padx=20, pady=(10,20), sticky="ew")
        self.progress_bar.set(0); self.progress_bar.start()
        self.log_queue = queue.Queue()
        self.after(100, self.process_log_queue)
        target_func = self.run_import_worker if for_import else self.run_resolve_worker
        threading.Thread(target=target_func, daemon=True).start()

    def update_log(self, message): self.log_queue.put(message)
    def process_log_queue(self):
        while not self.log_queue.empty():
            message = self.log_queue.get_nowait()
            self.log_box.configure(state="normal")
            self.log_box.insert("end", message + "\n")
            self.log_box.configure(state="disabled")
            self.log_box.see("end")
        self.after(100, self.process_log_queue)
        
    def run_resolve_worker(self):
        self.app.resolved_packages = core.resolve_dependencies(self.app.user_selections, self.app.mrtk_releases_json, self.update_log)
        self.app.after(0, self.app.show_confirmation_frame)
        
    def run_import_worker(self):
        core.download_and_apply_packages(self.app.project_path, self.app.resolved_packages, self.app.mrtk_releases_json, self.app.openxr_selections, self.update_log)
        self.app.after(0, self.app.show_completion_frame)

class CompletionFrame(ctk.CTkFrame):
    def __init__(self, master, **kwargs):
        super().__init__(master, **kwargs)
        self.app = master
        self.grid_columnconfigure(0, weight=1); self.grid_rowconfigure(0, weight=1)
        container = ctk.CTkFrame(self, fg_color="transparent"); container.place(relx=0.5, rely=0.5, anchor="center")
        ctk.CTkLabel(container, text="Unity Project Updated", font=ctk.CTkFont(size=24, weight="bold")).pack(pady=10)
        ctk.CTkLabel(container, text=f"{self.app.project_path.name} has been updated.\nPlease return to Unity to load the imported features.").pack(pady=5)
        button_frame = ctk.CTkFrame(self, fg_color="transparent"); button_frame.grid(row=1, column=0, padx=20, pady=20, sticky="e")
        ctk.CTkButton(button_frame, text="Start Over", command=self.app.show_project_select_frame).pack(side="left", padx=5)
        ctk.CTkButton(button_frame, text="Exit", command=self.app.quit).pack(side="left", padx=5)

if __name__ == "__main__":
    app = App()
    app.mainloop()