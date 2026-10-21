# Windows x86 构建

使用 Visual Studio 2022 的 **x86 Native Tools Command Prompt**（或调用 `VsDevCmd.bat -arch=x86 -host_arch=amd64`）。需要 CMake、Ninja、Python 和 Windows SDK。

中文版 MSVC 请在首次配置前执行 `chcp 65001`，保证 CMake 正确识别 `/showIncludes` 的中文依赖前缀。若 `CMakeFiles/rules.ninja` 的 `msvc_deps_prefix` 已出现乱码，应重新配置（`cmake --fresh` 加上原有配置参数）并执行 `cmake --build build-x86 --clean-first`；否则头文件变化可能无法触发相关文件重编译。

Qt 6.9 的官方 Windows 支持配置不包含 x86。本构建使用未经修改的 Qt 6.9.1 qtbase 源码自行编译；不是 Windows 7 兼容版本，也不代表已在 32 位 Windows 系统上验收。

## 1. 构建 Qt

下载 [Qt 6.9.1 qtbase 源码](https://download.qt.io/archive/qt/6.9/6.9.1/submodules/qtbase-everywhere-src-6.9.1.tar.xz)，SHA-256：

```text
40caedbf83cc9a1959610830563565889878bc95f115868bbf545d1914acf28e
```

解压到 `tools/x86/qtbase-everywhere-src-6.9.1`。以下命令在仓库根目录执行，安装路径替换为自己的绝对路径：

```bat
chcp 65001
cmake -S tools/x86/qtbase-everywhere-src-6.9.1 -B tools/x86/qt-build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=F:/work/zip/tools/x86/qt ^
  -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_BENCHMARKS=OFF ^
  -DQT_FEATURE_sql=OFF -DQT_FEATURE_dbus=OFF -DQT_FEATURE_opengl=ON -DINPUT_opengl=desktop ^
  -DFEATURE_opengl_desktop=ON -DFEATURE_opengl_dynamic=OFF ^
  -DQT_FEATURE_vulkan=OFF -DQT_FEATURE_printsupport=OFF
cmake --build tools/x86/qt-build --parallel 8
cmake --install tools/x86/qt-build
```

## 2. 构建程序

从 [7-Zip 24.09 x86 安装包](https://www.7-zip.org/a/7z2409.exe) 提取 32 位 `7z.dll` 和 `7z.sfx` 到 `tools/x86/7zip`。不要使用 `Program Files/7-Zip` 中的 64 位 DLL。

```bat
chcp 65001
cmake -S . -B build-x86 -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH=F:/work/zip/tools/x86/qt ^
  -DZIPDESK_7Z_DLL=F:/work/zip/tools/x86/7zip/7z.dll ^
  -DWINDEPLOYQT_EXECUTABLE=F:/work/zip/tools/x86/qt/bin/windeployqt.exe
cmake --build build-x86 --parallel 8
ctest --test-dir build-x86 --output-on-failure
```

首次配置需要联网下载 bit7z 和对应的 7-Zip 头文件。x86 与 x64 必须使用不同的构建目录。

## 3. 打包

在 PowerShell 中执行，运行库目录必须选择 `x86`：

```powershell
.\scripts\package.ps1 -Architecture x86 -BuildDirectory build-x86 `
  -RuntimeDirectory 'C:\Path\To\VC\Redist\MSVC\14.xx\x86\Microsoft.VC143.CRT' `
  -Iscc 'C:\Path\To\Inno Setup\ISCC.exe'
```

产物为 `dist/QingXia-0.2.7-windows-x86-setup.exe`、`dist/QingXia-0.2.7-windows-x86-portable.zip` 和 `dist/SHA256SUMS-0.2.7-x86.txt`。脚本会检查所有 EXE、DLL、SFX 的 PE 架构，拒绝混入 x64 依赖；已有同名发布包不会被覆盖。

## 本地验证记录（2026-10-12）

- MSVC 19.44、Qt 6.9.1、7-Zip 24.09；Qt 源码未修改。
- 19 项 CTest 全部通过，包括服务层、窗口、解压结果、智能解压和移动流程。
- 安装包编译通过，分发目录内全部 EXE/DLL/SFX 的 PE Machine 为 I386。
- 将免安装 ZIP 解压到新目录，PATH 仅保留 Windows 系统目录，创建 ZIP、完整性校验、解压和文件 SHA-256 比对全部通过。
- 验证环境为当前 64 位 Windows 的 32 位运行环境；未执行安装/卸载或原生 32 位 Windows 验收。
