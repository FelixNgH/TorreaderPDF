cmake_minimum_required(VERSION 3.25)
project(TorReader VERSION 2.1.3 LANGUAGES CXX)

# ── FormibPDF — Rust helper engine (low-res preview rendering) ──────────────
include(FetchContent)
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG        v0.5
)
FetchContent_MakeAvailable(Corrosion)

set(CARGO_TARGET_DIR "${CMAKE_BINARY_DIR}/formibpdf-target" CACHE STRING
    "Cargo target dir (short path avoids MSVC PDB path-length limit on Windows)")
corrosion_import_crate(
    MANIFEST_PATH "${CMAKE_SOURCE_DIR}/src/formibpdf/Cargo.toml"
    PROFILE release
)
set_target_properties(formibpdf-static PROPERTIES
    CARGO_ENV "CARGO_TARGET_DIR=${CARGO_TARGET_DIR}"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Qt6 ──────────────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS
    Core Widgets Gui Concurrent PrintSupport OpenGL OpenGLWidgets Network
)

# ── PDFium (bundled prebuilt, BSD 3-Clause — see third_party/pdfium/LICENSE) ─
# Source: https://github.com/bblanchon/pdfium-binaries
set(PDFIUM_DIR "${CMAKE_SOURCE_DIR}/third_party/pdfium" CACHE PATH "PDFium directory")
add_library(pdfium SHARED IMPORTED)
if(WIN32)
    set(_pdfium_dll "${PDFIUM_DIR}/bin/pdfium.dll")
    set(_pdfium_lib "${PDFIUM_DIR}/lib/pdfium.dll.lib")
    if(NOT EXISTS "${_pdfium_lib}")
        set(_pdfium_lib "${PDFIUM_DIR}/lib/pdfium.lib")
    endif()
    if(NOT EXISTS "${_pdfium_dll}" OR NOT EXISTS "${_pdfium_lib}")
        message(FATAL_ERROR "PDFium not found at ${PDFIUM_DIR} — see README.md")
    endif()
    set_target_properties(pdfium PROPERTIES
        IMPORTED_LOCATION "${_pdfium_dll}"
        IMPORTED_IMPLIB   "${_pdfium_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${PDFIUM_DIR}/include"
    )
else()
    find_library(PDFIUM_LIB NAMES pdfium PATHS "${PDFIUM_DIR}/lib" NO_DEFAULT_PATH)
    if(NOT PDFIUM_LIB)
        message(FATAL_ERROR "PDFium not found — see README.md")
    endif()
    set_target_properties(pdfium PROPERTIES
        IMPORTED_LOCATION "${PDFIUM_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${PDFIUM_DIR}/include"
    )
endif()

# ── QPDF (Apache-2.0) — system package or vcpkg ──────────────────────────────
find_library(QPDF_LIB NAMES qpdf
    HINTS "${CMAKE_SOURCE_DIR}/third_party/qpdf/lib"
    NO_DEFAULT_PATH)
find_path(QPDF_INCLUDE NAMES qpdf/QPDF.hh
    HINTS "${CMAKE_SOURCE_DIR}/third_party/qpdf/include"
    NO_DEFAULT_PATH)
if(NOT QPDF_LIB OR NOT QPDF_INCLUDE)
    find_package(qpdf CONFIG QUIET)
    if(NOT qpdf_FOUND)
        message(FATAL_ERROR "QPDF not found. Install with: vcpkg install qpdf  (Windows) or  apt install libqpdf-dev  (Linux)")
    endif()
    set(QPDF_TARGET qpdf::libqpdf)
else()
    add_library(qpdf::qpdf UNKNOWN IMPORTED)
    set_target_properties(qpdf::qpdf PROPERTIES
        IMPORTED_LOCATION "${QPDF_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${QPDF_INCLUDE}"
    )
    set(QPDF_TARGET qpdf::qpdf)
endif()

# ── OpenSSL (Apache-2.0) — OPTIONAL, enables "Sign PDF" (PKCS#7 detached) ────
# Not required to build: if OpenSSL dev headers aren't installed, TorReader
# still builds fully, just without the digital-signature feature.
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    set(TORREADER_ENABLE_SIGNER ON)
    message(STATUS "OpenSSL found — Sign PDF feature ENABLED")
else()
    message(STATUS "OpenSSL not found — Sign PDF feature DISABLED (install libssl-dev / vcpkg openssl to enable)")
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
    src/core/PdfPrinter.cpp

    # Annotations
    src/annotations/AnnotationLayer.cpp
    src/annotations/AnnotationTool.cpp
    src/annotations/AnnotationManager.cpp

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

if(TORREADER_ENABLE_SIGNER)
    list(APPEND SOURCES src/core/PdfSigner.cpp src/ui/SignDialog.cpp)
    list(APPEND HEADERS src/core/PdfSigner.h   src/ui/SignDialog.h)
endif()

# ── Executable ────────────────────────────────────────────────────────────────
if(WIN32)
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
    $<$<BOOL:${TORREADER_ENABLE_SIGNER}>:TORREADER_ENABLE_SIGNER>
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
    $<$<BOOL:${TORREADER_ENABLE_SIGNER}>:OpenSSL::Crypto>
    # Rust formibpdf image_decoder pulls in LittleCMS (ICC → sRGB) on Linux
    $<$<NOT:$<BOOL:${WIN32}>>:lcms2>
)

# ── Packaging (Windows portable) ──────────────────────────────────────────────
if(WIN32)
    add_custom_command(TARGET TorReader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFIUM_DIR}/bin/pdfium.dll"
            $<TARGET_FILE_DIR:TorReader>
        COMMENT "Copying pdfium.dll"
    )
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

    if(QPDF_LIB)
        get_filename_component(_qpdf_lib_dir "${QPDF_LIB}" DIRECTORY)
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
        add_custom_command(TARGET TorReader POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:qpdf::libqpdf>
                $<TARGET_FILE_DIR:TorReader>
            COMMENT "Copying qpdf DLL (from imported target)"
        )
    endif()
endif()
