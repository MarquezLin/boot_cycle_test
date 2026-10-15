# Third-party runtime components

The application uses dynamically linked Qt 6.10.2 (Core, Gui, Widgets, Network,
Svg and deployed plugins). Qt license information from the installed SDK is in
`Qt/`. Qt and its third-party component notices and source archives are available at:
https://doc.qt.io/qt-6.10/licenses-used-in-qt.html
https://download.qt.io/official_releases/qt/6.10/6.10.2/submodules/

Portable OCR is Tesseract 5.4.0.20240606, from the UB Mannheim Windows build:
https://github.com/UB-Mannheim/tesseract/wiki
https://github.com/UB-Mannheim/tesseract
Tesseract's Apache-2.0 license, authors and upstream README are in `Tesseract/`.
The exact distributed runtime files and SHA-256 hashes are in
`Tesseract/runtime-manifest.json`. Language-data licensing is also in
`bin/ocr/tessdata/LICENSE`.

The Windows OCR distribution includes independently licensed libraries (including
Leptonica, libarchive, GCC runtime and image/font/compression libraries).
Their upstream sources and licensing metadata can be located through the build
recipes used for Windows packages:
https://github.com/msys2/MINGW-packages
https://github.com/DanBloomberg/leptonica
https://github.com/libarchive/libarchive
https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html

Microsoft Visual C++ Redistributable is provided as its original installer,
`bin/vc_redist.x64.exe`; its license is presented by that installer.
The application's executable is not code signed.
