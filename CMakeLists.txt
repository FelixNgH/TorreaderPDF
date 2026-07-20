cmake_minimum_required(VERSION 3.25)
project(TorReader VERSION 2.0.0 LANGUAGES CXX)

# ── FormibPDF — Rust engine (Phase 2) ────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG        v0.5
)
FetchContent_MakeAvailable(Corrosion)

set(CARGO_TARGET_DIR "C:/tmp/formibpdf-target" CACHE STRING "Cargo target dir (short path to avoid MSVC PDB limit)")
corrosion_import_crate(
    MANIFEST_PATH "${CMAKE_SOURCE_DIR}/src/formibpdf/Cargo.toml"
    PROFILE release
)
# Override cargo target dir to avoid long-path PDB bug on Windows
set_target_properties(formibpdf-static PROPERTIES
    CARGO_ENV "CARGO_TARGET_DIR=${CARGO_TARGET_DIR}"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Output to build/bin
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Qt6 ──────────────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS
    Core Widgets Gui Concurrent PrintSupport OpenGL OpenGLWidgets Network
)

# ── PDFium (pdfium-binaries pre-built) ───────────────────────────────────────
# Download từ https://github.com/bblanchon/pdfium-binaries/releases
# Giải nén vào third_party/pdfium/
set(PDFIUM_DIR "${CMAKE_SOURCE_DIR}/third_party/pdfium" CACHE PATH "PDFium directory")
add_library(pdfium SHARED IMPORTED)
if(WIN32)
    set(_pdfium_dll "${PDFIUM_DIR}/bin/pdfium.dll")
    set(_pdfium_lib "${PDFIUM_DIR}/lib/pdfium.dll.lib")
    if(NOT EXISTS "${_pdfium_lib}")
        # fallback: plain pdfium.lib
        set(_pdfium_lib "${PDFIUM_DIR}/lib/pdfium.lib")
    endif()
    if(NOT EXISTS "${_pdfium_dll}" OR NOT EXISTS "${_pdfium_lib}")
        message(FATAL_ERROR "PDFium không tìm thấy tại ${PDFIUM_DIR}. Xem README để tải pdfium-binaries.")
    endif()
    set_target_properties(pdfium PROPERTIES
        IMPORTED_LOCATION "${_pdfium_dll}"
        IMPORTED_IMPLIB   "${_pdfium_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${PDFIUM_DIR}/include"
    )
else()
    find_library(PDFIUM_LIB NAMES pdfium PATHS "${PDFIUM_DIR}/lib" NO_DEFAULT_PATH)
    if(NOT PDFIUM_LIB)
        message(FATAL_ERROR "PDFium không tìm thấy. Xem README để tải pdfium-binaries.")
    endif()
    set_target_properties(pdfium PROPERTIES
        IMPORTED_LOCATION "${PDFIUM_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${PDFIUM_DIR}/include"
    )
endif()

# ── QPDF (Apache 2.0) ────────────────────────────────────────────────────────
# NO_DEFAULT_PATH: only look in third_party/ — prevents CMake picking up the
# vcpkg debug lib from the system search path when third_party/ is absent.
find_package(PkgConfig QUIET)
find_library(QPDF_LIB NAMES qpdf
    HINTS "${CMAKE_SOURCE_DIR}/third_party/qpdf/lib"
    NO_DEFAULT_PATH)
find_path(QPDF_INCLUDE NAMES qpdf/QPDF.hh
    HINTS "${CMAKE_SOURCE_DIR}/third_party/qpdf/include"
    NO_DEFAULT_PATH)
if(NOT QPDF_LIB OR NOT QPDF_INCLUDE)
    # Prefer vcpkg cmake config (handles Release/Debug DLLs correctly).
    find_package(qpdf CONFIG QUIET)
    if(NOT qpdf_FOUND)
        message(FATAL_ERROR "QPDF không tìm thấy. Cài bằng: vcpkg install qpdf hoặc apt install libqpdf-dev")
    endif()
    set(QPDF_TARGET qpdf::libqpdf)  # target name from vcpkg cmake config
else()
    add_library(qpdf::qpdf UNKNOWN IMPORTED)
    set_target_properties(qpdf::qpdf PROPERTIES
        IMPORTED_LOCATION "${QPDF_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${QPDF_INCLUDE}"
    )
    set(QPDF_TARGET qpdf::qpdf)
endif()

# ── Tesseract OCR (Apache 2.0) ───────────────────────────────────────────────
find_library(TESSERACT_LIB NAMES tesseract tesseract53
    HINTS "${CMAKE_SOURCE_DIR}/third_party/tesseract/lib")
find_path(TESSERACT_INCLUDE NAMES tesseract/baseapi.h
    HINTS "${CMAKE_SOURCE_DIR}/third_party/tesseract/include")
if(NOT TESSERACT_LIB)
    find_package(Tesseract CONFIG QUIET)
    if(NOT Tesseract_FOUND)
        message(WARNING "Tesseract không tìm thấy — OCR search raster sẽ bị tắt.")
        set(FELIXPDF_ENABLE_OCR OFF)
    endif()
else()
    add_library(Tesseract::libtesseract UNKNOWN IMPORTED)
    set_target_properties(Tesseract::libtesseract PROPERTIES
        IMPORTED_LOCATION "${TESSERACT_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${TESSERACT_INCLUDE}"
    )
    set(FELIXPDF_ENABLE_OCR ON)
endif()

# ── Sources ───────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp

    # Core PDF engine
    src/core/PdfDocument.cpp
    src/core/PdfRenderer.cpp
    src/core/TileCacheFile.cpp
    src/core/ThumbnailRenderPool.cpp
    src/core/PdfEditor.cpp
    src/core/TextSearch.cpp
    src/core/UpdateChecker.cpp
    src/core/GoogleAuth.cpp
    src/core/Translator.cpp

    # Annotations
    src/annotations/AnnotationLayer.cpp
    src/annotations/AnnotationTool.cpp
    src/annotations/AnnotationManager.cpp

    # Print
    src/core/PdfPrinter.cpp

    # UI
    src/ui/MainWindow.cpp
    src/ui/PdfView.cpp
    src/ui/PdfGpuView.cpp
    src/ui/ThumbnailPanel.cpp
    src/ui/SearchPanel.cpp
    src/ui/ContinuousView.cpp
    src/ui/ToolBar.cpp
    src/ui/MergeDialog.cpp
    src/ui/PrintDialog.cpp
    src/ui/AboutDialog.cpp
    src/ui/NoteInputDialog.cpp
    src/ui/UpdateBanner.cpp
    src/ui/TranslationPopup.cpp
    src/ui/NotificationBar.cpp
)

set(HEADERS
    src/core/PdfDocument.h
    src/core/PdfRenderer.h
    src/core/TileCacheFile.h
    src/core/PdfEditor.h
    src/core/PdfPrinter.h
    src/core/TextSearch.h
    src/core/UpdateChecker.h
    src/core/GoogleAuth.h
    src/core/Translator.h
    src/annotations/AnnotationLayer.h
    src/annotations/AnnotationTool.h
    src/annotations/AnnotationTypes.h
    src/annotations/AnnotationManager.h
    src/ui/MainWindow.h
    src/ui/PdfView.h
    src/ui/PdfGpuView.h
    src/ui/ThumbnailPanel.h
    src/ui/SearchPanel.h
    src/ui/ContinuousView.h
    src/ui/ToolBar.h
    src/ui/MergeDialog.h
    src/ui/PrintDialog.h
    src/ui/AboutDialog.h
    src/ui/NoteInputDialog.h
    src/ui/UpdateBanner.h
    src/ui/TranslationPopup.h
    src/ui/NotificationBar.h
)

if(FELIXPDF_ENABLE_OCR)
    list(APPEND SOURCES src/core/OcrEngine.cpp)
    list(APPEND HEADERS src/core/OcrEngine.h)
endif()

# ── Executable ────────────────────────────────────────────────────────────────
if(WIN32)
    # Windows: ẩn console window, embed .ico into exe (Explorer + taskbar pinned icon)
    add_executable(TorReader WIN32
        ${SOURCES} ${HEADERS}
        resources/resources.qrc
        resources/TorReader.rc
    )
else()
    add_executable(TorReader ${SOURCES} ${HEADERS} resources/resources.qrc)
endif()

target_compile_definitions(TorReader PRIVATE
    FELIXPDF_VERSION="${PROJECT_VERSION}"
    $<$<BOOL:${FELIXPDF_ENABLE_OCR}>:FELIXPDF_ENABLE_OCR>
)

target_include_directories(TorReader PRIVATE src src/formibpdf)

target_link_libraries(TorReader PRIVATE
    formibpdf-static
    Qt6::Core
    Qt6::Widgets
    Qt6::Gui
    Qt6::Concurrent
    Qt6::PrintSupport
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Network
    pdfium
    ${QPDF_TARGET}
    $<$<BOOL:${FELIXPDF_ENABLE_OCR}>:Tesseract::libtesseract>
    # Rust formibpdf image_decoder pulls in LittleCMS (ICC → sRGB) on Linux
    $<$<NOT:$<BOOL:${WIN32}>>:lcms2>
)

# ── Packaging (Windows portable) ──────────────────────────────────────────────
if(WIN32)
    # Copy PDFium DLL
    add_custom_command(TARGET TorReader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFIUM_DIR}/bin/pdfium.dll"
            $<TARGET_FILE_DIR:TorReader>
        COMMENT "Copying pdfium.dll"
    )
    # windeployqt để bundle Qt DLLs
    find_program(WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(WINDEPLOYQT)
        add_custom_command(TARGET TorReader POST_BUILD
            COMMAND ${WINDEPLOYQT}
                --no-translations
                --no-system-d3d-compiler
                $<TARGET_FILE:TorReader>
            COMMENT "Running windeployqt"
        )
    endif()

    # Copy QPDF DLL (vcpkg x64-windows: DLL lives next to the .lib import lib)
    if(QPDF_LIB)
        get_filename_component(_qpdf_lib_dir "${QPDF_LIB}" DIRECTORY)
        # vcpkg layout: lib/ → ../bin/
        file(GLOB _qpdf_dlls
            "${_qpdf_lib_dir}/../bin/qpdf*.dll"
            "${_qpdf_lib_dir}/qpdf*.dll"
        )
        foreach(_dll ${_qpdf_dlls})
            add_custom_command(TARGET TorReader POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_dll}" $<TARGET_FILE_DIR:TorReader>
                COMMENT "Copying ${_dll}"
            )
        endforeach()
    elseif(qpdf_FOUND)
        # Installed via find_package(qpdf CONFIG) — target carries DLL path
        add_custom_command(TARGET TorReader POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:qpdf::libqpdf>
                $<TARGET_FILE_DIR:TorReader>
            COMMENT "Copying qpdf DLL (from imported target)"
        )
    endif()
endif()
