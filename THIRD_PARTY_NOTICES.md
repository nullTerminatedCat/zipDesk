# Third-party components

QingXia 0.1.0 Windows x64 binaries include these components. Their licenses apply independently of the QingXia application code, which is licensed under the MIT License in the repository's `LICENSE` file.

| Component | Version | License / source |
| --- | --- | --- |
| Qt (Core, Gui, Widgets, Network and deployed plugins) | 6.9.1 | LGPL v3; license texts in `licenses/Qt-*`. Corresponding source: https://download.qt.io/archive/qt/6.9/6.9.1/single/qt-everywhere-src-6.9.1.tar.xz |
| 7-Zip (`7z.dll`, `7z.sfx`) | 24.09 | LGPL with unRAR restrictions and BSD portions; see `licenses/7-Zip.txt` and `licenses/LGPL-2.1.txt`. Source: https://www.7-zip.org/a/7z2409-src.7z |
| bit7z | 4.0.12 | MPL 2.0, see `licenses/bit7z-MPL-2.0.txt`. Unmodified source: https://github.com/rikyoz/bit7z/tree/v4.0.12 |
| Microsoft Visual C++ runtime and DirectX runtime/compiler libraries | Files deployed by Visual Studio / Qt | Microsoft redistributable components, subject to their respective Microsoft terms. |

Qt is dynamically linked; its DLLs remain separate in the application directory. The application's source and build instructions are available at https://github.com/huanglin2/zipDesk. No restriction is imposed here on reverse engineering for debugging modifications to LGPL-covered libraries.

In the source repository, license texts are stored under `docs/licenses/`; distributed packages put them under `licenses/`.
