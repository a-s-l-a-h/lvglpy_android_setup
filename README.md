

# LvglPy Android Builder 📱🐍

A graphical, template-based build automation tool written in Python (Tkinter). This tool is designed to cross-compile [lvglpy](https://github.com/a-s-l-a-h/lvglpy) (LVGL for Python) into native Android shared libraries (`.so`) and package them into Python Wheels (`.whl`) using **Chaquopy** targets and the **Android NDK**.

## ✨ Features
* **Automated Environment Setup:** Automatically downloads and extracts the correct Chaquopy development targets (Python headers and libs) for your chosen Python version.
* **Git & Codegen Integration:** Automatically clones the `lvglpy` repository and runs its required Python codegen (`pycparser`, `jinja2`).
* **Multi-ABI Support:** Build for `arm64-v8a`, `armeabi-v7a`, `x86`, and `x86_64` in a single click.
* **Template Engine:** Injects custom `CMakeLists.txt`, `lv_conf.h`, and Android specific C++ wrappers into the build process automatically.
* **Wheel Packaging:** Directly outputs `.whl` files that can be installed inside an Android Python environment (like Chaquopy).

---

## 📋 Prerequisites

Before using this tool, ensure your host machine has the following installed:

1. **Python 3.x** (with `tkinter` support).
2. **Git** (Required for cloning the `lvglpy` repository).
3. **Android NDK** (Usually installed via Android Studio at `~/Android/Sdk/ndk/` on Linux/Mac or `%LOCALAPPDATA%\Android\Sdk\ndk\` on Windows).
4. **CMake** (Often bundled with the Android SDK/NDK or installed system-wide).

---

## 📁 Directory Structure

Ensure your directory looks exactly like this before running the script. The script relies heavily on the `lvglpy_build_help_files` directory containing the templates.

```text
.
├── lvglpy_android_setup.py           # <- The main GUI application
├── lvglpy_build_help_files/          # <- REQUIRED TEMPLATE FOLDER
│   ├── CMakeLists.txt                # Custom CMake wrapper for Android
│   ├── DroidbindInit.cmake           # Chaquopy CMake initialization
│   ├── lv_conf.h                     # LVGL configuration file
│   └── backends/
│       └── android_input_sw.cpp      # Android specific C++ input wrapper
└── README.md
```

---

## 🚀 How to Use

### 1. Launch the Builder
Open a terminal in the directory containing the script and run:
```bash
python3 lvglpy_android_setup.py
```

### 2. Configure the Settings (Left Panel)
* **Output Location:** Choose where the workspace and final built files will be saved.
* **Module & Wheel:** Define the module name (`lvglpy`), wheel name, and the internal library version.
* **Chaquopy Target:** Set the target Python version (e.g., `3.12.0-0` or `3.8.0-0`). *This must match the Python version your Android app uses.*
* **NDK Path:** Click `...` to locate your Android NDK folder. The script will try to auto-detect this, but ensure it points to a specific NDK version folder (e.g., `/home/user/Android/Sdk/ndk/25.1.8937393`).
* **Target ABIs:** Select the Android architectures you want to compile for (usually `arm64-v8a` and `armeabi-v7a` are enough for modern devices).
* **Output Format:** Choose whether you want standalone `.so` files, `.whl` files, or both.

### 3. Build Process (Right Panel Buttons)

Follow these steps in order using the buttons on the right side of the screen:

#### Step A: ⚙ Prepare Workspace
Clicking this button will:
1. Create your workspace directory.
2. Download and extract the Chaquopy development headers for your selected Python version.
3. Copy the templates from `lvglpy_build_help_files` into the workspace.
4. Clone the `lvglpy` repository from GitHub.
5. Install necessary host-packages (`pycparser`, `jinja2`) and run the `lvglpy` C++ code generators.

*Wait for the console to say **"✓ Workspace Ready!"***

#### Step B: ▶ Compile & Pack
Clicking this button will:
1. Invoke CMake using the Android NDK toolchain.
2. Cross-compile the `lvglpy` source code into `.so` files for every selected ABI.
3. Package the `.so` files and necessary metadata into standard Python `.whl` files.

*Wait for the console to say **"✓ BUILD COMPLETE"***.

#### Step C: Retrieve your Wheels
Navigate to your configured Workspace directory (e.g., `~/lvglpy_android_workspace/lvglpy_build/dist/`). You will find your `.whl` files there, ready to be copied to your Android project.

---

## 🎛️ Additional Controls

* **↻ Update Templates:** If you manually modify the files inside `lvglpy_build_help_files/` (like changing a setting in `lv_conf.h`), click this to push those changes to your active workspace without redownloading everything.
* **💾 Save:** Saves your current GUI inputs to `lvglpy_builder_settings.json` so they persist the next time you open the app.
* **✕ Clear Log:** Clears the built-in terminal window.

---

## ⚠️ Troubleshooting

* **Missing `lv_conf.h` or template errors:** Ensure the `lvglpy_build_help_files` folder is in the exact same directory as `lvglpy_android_setup.py`.
* **CMake Build Fails:** Check your NDK path. It must point to the *inside* of a specific NDK version folder, not just the root `ndk/` directory.
* **Pip Install Errors during Workspace Prep:** The script attempts to run `pip install pycparser jinja2 --break-system-packages`. If your environment strictly forbids this, you may need to install them manually (`pip install pycparser jinja2`) or run the GUI tool inside a Python Virtual Environment (`venv`).