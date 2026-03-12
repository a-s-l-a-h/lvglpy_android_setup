# ⬡ Droidbind — Nanobind for Android

**Droidbind** is a GUI tool that automates building C++ Python extensions for Android using **nanobind**. 

It handles the complex task of cross-compiling C++ code, linking against the correct Python headers for Android, managing CMake toolchains, and packaging the result into a standard `.whl` (Wheel) file that you can drop directly into **Chaquopy**.

---

## ✅ Prerequisites

Before running the tool, ensure you have:

1.  **Python 3.10+** installed on your computer.
2.  **Git** installed and available in your system PATH.
3.  **Android NDK (Side-by-side) r23+**:
    *   *Where to find:* Android Studio -> SDK Manager -> SDK Tools -> NDK (Side-by-side).
    *   *Note:* You need the path to this folder (e.g., `C:\Users\You\AppData\Local\Android\Sdk\ndk\25.1.8937393`).
4.  **CMake**: Usually installed via Android Studio SDK Tools, or system CMake.

---

## 🚀 Quick Start

1.  Run the script:
    ```bash
    python droidbind.py
    ```
2.  **Generate Template**: Fill in your project details and click "Generate Template". This creates a `cpp` folder with a sample `mylibrary.cpp`.
3.  **Write Code**: Edit `cpp/src/mylibrary.cpp` with your nanobind code.
4.  **Build**: Click "Build".
5.  **Install**: Copy the generated `.whl` file to your Android project's `app/libs` folder.

---

## ⚙️ GUI Configuration Guide

Here is a detailed explanation of every field in the Droidbind interface.

### 1. Project & Module
*   **Project Location**: The folder on your computer where the C++ source code and build artifacts will be saved.
*   **Project Folder Name**: The name of the folder created inside the location above.
*   **Module Name**: This is the name you type in Python to use the library.
    *   *Example:* If you set this to `mylibrary`, in Python you will type `import mylibrary`.
*   **Library Version**: Your version number (e.g., `1.0`, `0.5.2`).

### 2. Versions (Critical)
This section tells Droidbind which Python version to target.

*   **Chaquopy Target Version**: 
    *   This determines the Python version and the Android-specific headers.
    *   **How to find available versions:**
        1.  Open this URL in your browser:  
            👉 **[https://repo1.maven.org/maven2/com/chaquo/python/target/](https://repo1.maven.org/maven2/com/chaquo/python/target/)**
        2.  Look at the list of folders. 
        3.  *Example:* `3.12.0-2` means Python 3.12. `3.8.16-1` means Python 3.8.
        4.  Copy the folder name exactly into the Droidbind field.

*   **nanobind Version**: 
    *   The Git tag for nanobind. Defaults to `v2.12.0`. You usually don't need to change this unless you want a specific feature.

*   **Min Android SDK**:
    *   The minimum Android API level your app supports (e.g., `24` for Android 7.0). This must match `minSdk` in your `build.gradle`.

### 3. NDK & Architecture
*   **NDK Path**: Click the `...` button to browse to your Android NDK folder.
    *   *Windows:* `%LOCALAPPDATA%\Android\Sdk\ndk\<version>`
    *   *Mac:* `~/Library/Android/sdk/ndk/<version>`
*   **Target ABIs**:
    *   **arm64-v8a**: (Recommended) Used by almost all modern physical Android phones.
    *   **x86_64**: (Recommended) Used by the Android Emulator on PC/Mac.
    *   *armeabi-v7a*: For very old phones (32-bit).
    *   *x86*: For very old emulators.

### 4. Output Format
*   **whl (Wheel)**: (Recommended) Generates a standard Python package file. Easiest to install.
*   **so (Shared Object)**: Generates the raw `libmylibrary.so` file. Only use this if you want to manually manage `jniLibs`.

---
## you can use ( copy paste the whl on this android project (for demo))
[link](https://github.com/a-s-l-a-h/nanobindChaqopy/tree/v0.09)

## 📦 How to use the Output (Integration)

Once you click **Build**, Droidbind generates a `.whl` file in the `dist/` folder inside your project.

### Step 1: Copy File
Copy the generated file (e.g., `mylibrary-1.0-cp312-cp312-android_24_arm64_v8a.whl`) into your Android Studio project at:
`app/libs/`

### Step 2: Configure `build.gradle`
Edit `app/build.gradle` to tell Chaquopy to look in the `libs` folder.

```
groovy
plugins {
    id 'com.android.application'
    id 'com.chaquo.python'
}

android {
    // ... setup ...
    defaultConfig {
        ndk {
            // Ensure these match what you selected in Droidbind
            abiFilters "arm64-v8a", "x86_64"
        }
    }
}

chaquopy {
    defaultConfig {
        // Must match the version you selected in Droidbind (e.g., 3.12)
        version = "3.12" 
        buildPython("/usr/bin/python3") // Path to python on your PC

        pip {
            // 1. Tell pip to look in the libs folder (projectDir is automatic)
            options("--find-links", "${project.projectDir}/libs")
            
            // 2. Install your library by name (pip finds the right .whl automatically)
            install("mylibrary==1.0")
        }
    }
} 
```



## 🛠 Advanced Details

### 1. CMake Superiority (The "Root" Project)
The `cpp/CMakeLists.txt` file generated by Droidbind acts as the **Root Project**.
*   **Control**: It has complete control over compilation flags, include directories, and linking.
*   **Submodules**: You can add any C++ library (like `fmt`, `nlohmann/json`, `bullet3`) by placing the source code in `cpp/external/` and using `add_subdirectory()` in your CMakeLists.
*   **Inheritance**: Any flags set in the root CMakeLists (like C++17 standard) are automatically inherited by submodules.

### 2. The Injection Hook (`DroidbindInit.cmake`)
Droidbind uses a sophisticated injection mechanism to ensure build stability across all architectures:
*   It generates a file called `DroidbindInit.cmake` inside the build folder.
*   This file is injected via `CMAKE_PROJECT_TOP_LEVEL_INCLUDES`.
*   **What it does**: It pre-defines variables like `Python_FOUND`, `Python_INCLUDE_DIRS`, and `Python_LIBRARIES` pointing strictly to the **Android** version of Python (extracted from the Chaquopy maven targets).
*   **Why it matters**: This prevents third-party submodules (like `pybind11` or others that call `find_package(Python)`) from accidentally detecting and linking against your **Desktop** Python installation, which would cause the app to crash on Android.

### 3. Static Linking (`c++_static`)
By default, Droidbind configures the build with:
-DANDROID_STL=c++_static
*   **The Problem**: Chaquopy and standard Android builds often rely on `libc++_shared.so`. However, if your extension is built separately, the Android loader might fail to find this shared library at runtime (causing `dlopen failed: library "libc++_shared.so" not found`).
*   **The Fix**: Droidbind compiles the C++ Standard Library **statically** into your `.so` file. This makes your extension self-contained and "crash-proof" regarding standard library dependencies.

### 4. Handling Dependencies
*   **C++ Dependencies**: Managed via `cpp/CMakeLists.txt`. You must download headers/source and link them manually or via `FetchContent`.


##sample default values .. droidbind_settings.json  ( if you like then you can create this file and past below code )

```
{
  "proj_loc": "/home/",
  "proj_name": "test",
  "mod_name": "mylibrary",
  "lib_ver": "1.0",
  "chaquo_ver": "3.12.0-0",
  "nb_ver": "v2.12.0",
  "min_sdk": "21",
  "ndk_path": "/home/Android/Sdk/ndk/28.",
  "out_fmt": "both",
  "abis": {
    "arm64-v8a": true,
    "armeabi-v7a": false,
    "x86": false,
    "x86_64": true
  }
} 
```
