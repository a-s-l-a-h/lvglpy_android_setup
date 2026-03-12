#!/usr/bin/env python3
"""
LvglPy Android Builder - Template Based
Builds lvglpy nanobind wheels for Android natively using Chaquopy targets.
"""
import tkinter as tk
from tkinter import ttk, filedialog, scrolledtext, messagebox
import threading, os, sys, json, zipfile, shutil, subprocess, platform
from pathlib import Path
from urllib.request import urlretrieve, urlopen

# ── Constants ────────────────────────────────────────────────────────────────
MAVEN_BASE   = "https://repo1.maven.org/maven2/com/chaquo/python/target"
LVGLPY_GIT   = "https://github.com/a-s-l-a-h/lvglpy.git"
ALL_ABIS     = ["arm64-v8a", "armeabi-v7a", "x86", "x86_64"]
BASE_DIR     = Path(__file__).parent
SETTINGS_FILE = BASE_DIR / "lvglpy_builder_settings.json"
HELP_FILES_DIR = BASE_DIR / "lvglpy_build_help_files"

# ── Main App ──────────────────────────────────────────────────────────────────
class App:
    def __init__(self, root):
        self.root = root
        root.title("LvglPy Android Builder")
        self._settings = self._load_settings()
        self._build_ui()
        self._center()
        root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _load_settings(self):
        if SETTINGS_FILE.exists():
            try: return json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
            except: pass
        return {}

    def _save_all_settings(self):
        self._settings.update({
            "proj_loc": self.v_proj_loc.get(), "proj_name": self.v_proj_name.get(),
            "mod_name": self.v_name.get(), "whl_name": self.v_whl_name.get(),
            "lib_ver": self.v_ver.get(), "chaquo_ver": self.v_chaquo.get(),
            "dl_repo": self.v_dl_repo.get(), "min_sdk": self.v_sdk.get(),
            "ndk_path": self.v_ndk.get(), "out_fmt": self.v_out_fmt.get(),
            "abis": {abi: var.get() for abi, var in self.abi_vars.items()},
        })
        try: SETTINGS_FILE.write_text(json.dumps(self._settings, indent=2), encoding="utf-8")
        except: pass

    def _on_close(self):
        self._save_all_settings()
        self.root.destroy()

    def _center(self):
        self.root.update_idletasks()
        w, h = self.root.winfo_width(), self.root.winfo_height()
        x, y = (self.root.winfo_screenwidth() - w) // 2, (self.root.winfo_screenheight() - h) // 2
        self.root.geometry(f"{w}x{h}+{x}+{y}")

    def _s(self, key, default): return self._settings.get(key, default)

    def _build_ui(self):
        self.root.minsize(1050, 600)
        self.root.geometry("1100x650")
        
        main_pane = ttk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        main_pane.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)
        
        left_frame = ttk.Frame(main_pane, padding=5)
        main_pane.add(left_frame, weight=1)
        self.left_canvas = tk.Canvas(left_frame, highlightthickness=0)
        lsb = ttk.Scrollbar(left_frame, orient="vertical", command=self.left_canvas.yview)
        self.l_scroll = ttk.Frame(self.left_canvas)
        self.l_scroll.bind("<Configure>", lambda e: self.left_canvas.configure(scrollregion=self.left_canvas.bbox("all")))
        self.cw = self.left_canvas.create_window((0, 0), window=self.l_scroll, anchor="nw")
        self.left_canvas.bind("<Configure>", lambda e: self.left_canvas.itemconfig(self.cw, width=e.width))
        self.left_canvas.configure(yscrollcommand=lsb.set)
        self.left_canvas.pack(side="left", fill="both", expand=True)
        lsb.pack(side="right", fill="y")
        self.left_canvas.bind_all("<MouseWheel>", lambda e: self.left_canvas.yview_scroll(-1*(e.delta//120), "units"))
        
        ttk.Label(self.l_scroll, text="⬡ LVGLPY BUILDER", font=("Helvetica", 14, "bold"), foreground="#007ACC").pack(anchor="w", pady=(0, 10))
        
        self._sec("OUTPUT LOCATION")
        self.v_proj_loc = self._fld("Workspace:", self._s("proj_loc", str(Path.home() / "lvglpy_android_workspace")), "dir")
        self.v_proj_name = self._fld("Project Folder:", self._s("proj_name", "lvglpy_build"))
        
        self._sec("MODULE & WHEEL")
        self.v_name = self._fld("Module Name (.so):", self._s("mod_name", "lvglpy"))
        self.v_whl_name = self._fld("Wheel Name:", self._s("whl_name", "lvglpy"))
        self.v_ver = self._fld("Library Version:", self._s("lib_ver", "1.0"))
        
        self._sec("VERSIONS & NDK")
        self.v_chaquo = self._fld("Chaquopy Target:", self._s("chaquo_ver", "3.12.0-0"))
        
        self.v_dl_repo = tk.BooleanVar(value=self._s("dl_repo", True))
        ttk.Checkbutton(self.l_scroll, text="Clone lvglpy + Run Codegen (Requires Git & Host Python)", variable=self.v_dl_repo).pack(anchor="w", padx=10)
        
        self.v_sdk = self._fld("Min Android SDK:", self._s("min_sdk", "21"))
        self.v_ndk = self._fld("NDK Path:", self._s("ndk_path", self._find_ndk()), "dir")
        
        self._sec("TARGET ABIs")
        f = ttk.Frame(self.l_scroll)
        f.pack(fill="x", padx=10, pady=2)
        self.abi_vars = {}
        for abi in ALL_ABIS:
            self.abi_vars[abi] = tk.BooleanVar(value=self._s("abis", {}).get(abi, abi in ("arm64-v8a", "armeabi-v7a", "x86_64")))
            ttk.Checkbutton(f, text=abi, variable=self.abi_vars[abi]).pack(side="left", padx=5)
        
        self._sec("OUTPUT FORMAT")
        self.v_out_fmt = tk.StringVar(value=self._s("out_fmt", "both"))
        f2 = ttk.Frame(self.l_scroll)
        f2.pack(fill="x", padx=10, pady=5)
        for v, l in [("whl", ".whl"), ("so", ".so"), ("both", "both")]:
            ttk.Radiobutton(f2, text=l, variable=self.v_out_fmt, value=v).pack(side="left", padx=5)
        
        # Right Panel
        rf = ttk.Frame(main_pane, padding=5)
        main_pane.add(rf, weight=1)
        bb = ttk.Frame(rf)
        bb.pack(fill="x", pady=(0, 5))
        
        ttk.Button(bb, text="⚙ Prepare Workspace", command=self._gen).pack(side="left", padx=2)
        ttk.Button(bb, text="▶ Compile & Pack", command=self._build_cb).pack(side="left", padx=2)
        ttk.Button(bb, text="↻ Update Templates", command=self._btn_update_templates).pack(side="left", padx=2)
        ttk.Button(bb, text="💾 Save", command=lambda: [self._save_all_settings(), self._log("💾 Saved", "ok")]).pack(side="left", padx=2)
        ttk.Button(bb, text="✕ Clear Log", command=lambda: [self.log.configure(state="normal"), self.log.delete("1.0", "end"), self.log.configure(state="disabled")]).pack(side="right", padx=2)
        
        self.log = scrolledtext.ScrolledText(rf, font=("Consolas", 9), wrap=tk.WORD, state="disabled")
        self.log.pack(fill="both", expand=True)
        self.log.tag_config("ok", foreground="green")
        self.log.tag_config("err", foreground="red")
        self.log.tag_config("w", foreground="orange")
        self.log.tag_config("a", foreground="blue")
        self.log.tag_config("a2", foreground="purple")
        self.log.tag_config("d", foreground="#555555")

        self._log("⬡ LvglPy Android Builder Ready (Template Engine).", "ok")

    def _sec(self, title):
        ttk.Separator(self.l_scroll, orient="horizontal").pack(fill="x", pady=8)
        ttk.Label(self.l_scroll, text=title, font=("Helvetica", 9, "bold")).pack(anchor="w", padx=5)

    def _fld(self, label, default, mode="file"):
        fr = ttk.Frame(self.l_scroll)
        fr.pack(fill="x", pady=2, padx=10)
        ttk.Label(fr, text=label, width=18).grid(row=0, column=0, sticky="w")
        var = tk.StringVar(value=default)
        ttk.Entry(fr, textvariable=var).grid(row=0, column=1, sticky="ew", padx=2)
        fr.columnconfigure(1, weight=1)
        if mode == "dir":
            ttk.Button(fr, text="...", width=3, command=lambda: [p:=filedialog.askdirectory(), var.set(p) if p else None]).grid(row=0, column=2, padx=2)
        return var

    def _find_ndk(self):
        for base in [Path.home() / "Android/Sdk/ndk", Path.home() / "Library/Android/sdk/ndk", Path.home() / "AppData/Local/Android/Sdk/ndk"]:
            if base.exists() and (v := sorted(base.iterdir(), reverse=True)): return str(v[0])
        return ""

    def _sel_abis(self): return [a for a in ALL_ABIS if self.abi_vars[a].get()]

    def _log(self, msg, tag=None):
        def _a():
            self.log.configure(state="normal")
            self.log.insert("end", msg + "\n", tag or "")
            self.log.see("end")
            self.log.configure(state="disabled")
        self.root.after(0, _a)

    def _run_cmd(self, cmd, name, cwd=None):
        self._log(f"\n▶ {name}:\n> {' '.join(cmd)}", "a")
        try:
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1, errors='replace', cwd=cwd)
            for line in p.stdout: self._log("  " + line.rstrip('\n'), "d")
            p.wait()
            return p.returncode
        except Exception as e:
            self._log(f"  ✗ Exception running {name}: {e}", "err")
            return -1

    # ── Template Engine ───────────────────────────────────────────────────
    def _apply_templates(self, cpp_dir):
        if not HELP_FILES_DIR.exists(): return False
        
        target_dir = cpp_dir.parent / "python-target"
        py_ver = ".".join(self.v_chaquo.get().split(".")[:2])
        abis = self._sel_abis()
        
        abi_paths = {}
        for abi in abis:
            ph = target_dir / abi / "include" / f"python{py_ver}" / "Python.h"
            lib = target_dir / abi / "jniLibs" / abi / f"libpython{py_ver}.so"
            if ph.exists() and lib.exists():
                abi_paths[abi] = {"include": (ph.parent).resolve().as_posix(), "lib_dir": (lib.parent).resolve().as_posix()}
        
        if not abi_paths: return False
        
        first = list(abi_paths.keys())[0]
        elifs = "".join([f'elseif(ANDROID_ABI STREQUAL "{a}")\n    set(DB_PYTHON_INCLUDE "{abi_paths[a]["include"]}")\n    set(DB_PYTHON_LIB_DIR "{abi_paths[a]["lib_dir"]}")\n' for a in list(abi_paths.keys())[1:]])
        
        tpl_cmake = (HELP_FILES_DIR / "CMakeLists.txt").read_text()
        tpl_cmake = tpl_cmake.replace("{{NAME}}", self.v_name.get().strip()).replace("{{PY_VER}}", py_ver)
        tpl_cmake = tpl_cmake.replace("{{FIRST_ABI}}", first).replace("{{FIRST_INC}}", abi_paths[first]["include"]).replace("{{FIRST_LIB}}", abi_paths[first]["lib_dir"])
        tpl_cmake = tpl_cmake.replace("{{ELIFS}}", elifs)
        (cpp_dir / "CMakeLists.txt").write_text(tpl_cmake)
        
        shutil.copy2(HELP_FILES_DIR / "lv_conf.h", cpp_dir / "lv_conf.h")
        backends_dir = cpp_dir / "backends"
        backends_dir.mkdir(exist_ok=True)
        shutil.copy2(HELP_FILES_DIR / "backends/android_input_sw.cpp", backends_dir / "android_input_sw.cpp")

        # --- NEW SAFETY FIX ---
        # Delete the lvglpy original lv_conf.h if it exists to prevent conflicts!
        bad_conf = cpp_dir / "lvglpy" / "lv_conf.h"
        if bad_conf.exists():
            bad_conf.unlink()
            
        return True

    def _btn_update_templates(self):
        proj = Path(self.v_proj_loc.get()) / self.v_proj_name.get()
        if not (proj / "cpp").exists():
            return messagebox.showerror("Error", "Workspace not prepared yet.")
        if self._apply_templates(proj / "cpp"):
            self._log("\n✓ Templates updated from lvglpy_build_help_files", "ok")
        else:
            self._log("\n✗ Failed to update templates (Missing Python targets?)", "err")

    # ── Workspace Preparation ─────────────────────────────────────────────
    def _gen(self):
        if not HELP_FILES_DIR.exists():
            return messagebox.showerror("Error", f"Missing folder:\n{HELP_FILES_DIR}\nPlease create it and add the templates.")
        self._save_all_settings()
        threading.Thread(target=self._do_gen, daemon=True).start()

    def _do_gen(self):
        proj = Path(self.v_proj_loc.get()) / self.v_proj_name.get()
        chaquo_v = self.v_chaquo.get().strip()
        py_ver = ".".join(chaquo_v.split(".")[:2])
        abis = self._sel_abis()
        
        cpp_dir, target_dir = proj / "cpp", proj / "python-target"
        cpp_dir.mkdir(parents=True, exist_ok=True)
        target_dir.mkdir(exist_ok=True)
        
        self._log(f"\n◈ Downloading Chaquopy targets ({chaquo_v}) ...", "a2")
        abi_paths = {}
        for abi in abis:
            abi_dest = target_dir / abi
            ph, lib = abi_dest / "include" / f"python{py_ver}" / "Python.h", abi_dest / "jniLibs" / abi / f"libpython{py_ver}.so"
            
            if ph.exists() and lib.exists():
                self._log(f"  ✓ {abi} already complete", "ok")
                abi_paths[abi] = True
            else:
                if abi_dest.exists(): shutil.rmtree(abi_dest)
                z = target_dir / f"target-{chaquo_v}-{abi}.zip"
                url = f"{MAVEN_BASE}/{chaquo_v}/{z.name}"
                self._log(f"  ↓ {abi} ({z.name}) ...", "a2")
                try: urlretrieve(url, z)
                except Exception as e: self._log(f"  ⚠ {abi} dl failed: {e}", "w"); continue
                
                ex = target_dir / f"_raw_{abi}"
                with zipfile.ZipFile(z, "r") as zf: zf.extractall(ex)
                src = ex if (ex / "include").exists() else [f for f in ex.iterdir() if f.is_dir()][0]
                shutil.copytree(src, abi_dest)
                shutil.rmtree(ex, ignore_errors=True); z.unlink(missing_ok=True)
                
                if (abi_dest / "include" / f"python{py_ver}" / "Python.h").exists():
                    abi_paths[abi] = True
                    self._log(f"  ✓ {abi} extracted", "ok")

        if not abi_paths: return self._log("✗ No ABIs ready.", "err")

        self._log(f"\n◈ Copying Templates from {HELP_FILES_DIR.name} ...", "a")
        if self._apply_templates(cpp_dir):
            self._log("  ✓ Wrapper CMake, lv_conf.h, android_input_sw.c copied", "ok")
        else:
            return self._log("✗ Failed to apply templates", "err")

        # Clone & Codegen
        lvglpy_dir = cpp_dir / "lvglpy"
        if self.v_dl_repo.get():
            if not lvglpy_dir.exists():
                if self._run_cmd(["git", "clone", "--recurse-submodules", LVGLPY_GIT, str(lvglpy_dir)], "Git Clone") != 0: return self._log("✗ clone failed.", "err")
            self._run_cmd([sys.executable, "-m", "pip", "install", "pycparser==2.22", "jinja2==3.1.4", "--break-system-packages"], "Pip Install")
            r1 = self._run_cmd([sys.executable, "codegen/parse_headers.py"], "Parse Headers", cwd=str(lvglpy_dir))
            r2 = self._run_cmd([sys.executable, "codegen/generate.py"], "Generate Sources", cwd=str(lvglpy_dir))
            if r1 != 0 or r2 != 0: return self._log("✗ Codegen failed.", "err")

        self._log("\n✓ Workspace Ready! Click ▶ Compile & Pack.", "ok")

    # ── Compilation & Wheel Packing ───────────────────────────────────────
    def _build_cb(self):
        if not self.v_ndk.get().strip() or not Path(self.v_ndk.get()).exists(): return messagebox.showerror("Error", "Valid NDK required")
        self._save_all_settings()
        threading.Thread(target=self._do_build, daemon=True).start()

    def _do_build(self):
        proj = Path(self.v_proj_loc.get()) / self.v_proj_name.get()
        so_name, whl_name, lib_ver = self.v_name.get().strip(), self.v_whl_name.get().strip(), self.v_ver.get().strip()
        py_ver = ".".join(self.v_chaquo.get().split(".")[:2])
        ndk_path, min_sdk = Path(self.v_ndk.get().strip()), self.v_sdk.get().strip()
        cpp_dir, so_dir, dist_dir = proj / "cpp", proj / "so_output", proj / "dist"
        
        host_py = Path(sys.executable).as_posix()
        if not (cpp_dir / "CMakeLists.txt").exists(): return self._log("✗ Run Prepare Workspace first.", "err")

        # AUTO-UPDATE TEMPLATES BEFORE BUILD
        self._log("\n◈ Updating Templates from help files...", "d")
        self._apply_templates(cpp_dir)

        self._log("\n◈ Compiling ...", "a")
        tc = ndk_path / "build/cmake/android.toolchain.cmake"
        built = []
        
        for abi in self._sel_abis():
            self._log(f"\n── {abi} ──", "a2")
            b_dir = cpp_dir / "cmake_build" / abi
            b_dir.mkdir(parents=True, exist_ok=True)
            inc_abs = (proj / "python-target" / abi / "include" / f"python{py_ver}").resolve().as_posix()
            
            # Process DroidbindInit.cmake
            t_init = (HELP_FILES_DIR / "DroidbindInit.cmake").read_text()
            t_init = t_init.replace("{{PY_VER}}", py_ver).replace("{{PY_MAJOR}}", py_ver.split(".")[0]).replace("{{PY_MINOR}}", py_ver.split(".")[1]).replace("{{INC_ABS}}", inc_abs).replace("{{HOST_PYTHON}}", host_py)
            (b_dir / "DroidbindInit.cmake").write_text(t_init)
            
            c_args = ["cmake", str(cpp_dir), f"-B{b_dir}", f"-DCMAKE_TOOLCHAIN_FILE={tc}", f"-DANDROID_ABI={abi}", f"-DANDROID_PLATFORM=android-{min_sdk}", "-DANDROID_STL=c++_static", "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES={(b_dir / 'DroidbindInit.cmake').as_posix()}"]
            if self._run_cmd(c_args, "CMake Config") != 0: continue
            if self._run_cmd(["cmake", "--build", str(b_dir), "--config", "Release"], "CMake Build") != 0: continue
            
            so_files = list(b_dir.rglob(f"{so_name}.so"))
            if not so_files: self._log(f"  ✗ {so_name}.so not generated.", "err"); continue
            
            (so_dir / abi).mkdir(parents=True, exist_ok=True)
            shutil.copy2(so_files[0], so_dir / abi / f"{so_name}.so")
            self._log(f"  ✓ {abi} compiled", "ok")
            built.append(abi)
        
        if not built: return self._log("\n✗ BUILD FAILED. No ABIs compiled.", "err")
        if self.v_out_fmt.get() in ("whl", "both"):
            dist_dir.mkdir(exist_ok=True)
            import hashlib, base64
            def _rec(name, data): return f"{name},sha256={base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b'=').decode()},{len(data)}"
            
            s_ver, s_name, ptag = lib_ver.replace("-", "_"), whl_name.replace("-", "_"), f"cp{py_ver.replace('.', '')}"
            d_info = f"{s_name}-{s_ver}.dist-info"
            
            for abi in built:
                atag = abi.replace("-", "_")
                wpath = dist_dir / f"{s_name}-{s_ver}-{ptag}-{ptag}-android_{min_sdk}_{atag}.whl"
                with zipfile.ZipFile(wpath, "w", compression=zipfile.ZIP_DEFLATED) as zf:
                    recs = []
                    d = (so_dir / abi / f"{so_name}.so").read_bytes()
                    zf.writestr(f"{so_name}.so", d); recs.append(_rec(f"{so_name}.so", d))
                    w_b = f"Wheel-Version: 1.0\nGenerator: lvglpy_builder\nRoot-Is-Purelib: false\nTag: {ptag}-{ptag}-android_{min_sdk}_{atag}\n".encode()
                    zf.writestr(f"{d_info}/WHEEL", w_b); recs.append(_rec(f"{d_info}/WHEEL", w_b))
                    m_b = f"Metadata-Version: 2.1\nName: {s_name}\nVersion: {s_ver}\n".encode()
                    zf.writestr(f"{d_info}/METADATA", m_b); recs.append(_rec(f"{d_info}/METADATA", m_b))
                    zf.writestr(f"{d_info}/RECORD", "\n".join(recs) + f"\n{d_info}/RECORD,,\n")
                self._log(f"\n📦 {wpath.name}", "ok")
        self._log("\n✓ BUILD COMPLETE", "ok")

if __name__ == "__main__":
    root = tk.Tk()
    App(root)
    root.mainloop()