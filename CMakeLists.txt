cmake_minimum_required(VERSION 3.25)
project(TorReader VERSION 2.3.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(TORREADER_NO_PDFIUM)
    set(CMAKE_AUTOMOC OFF)
    set(CMAKE_AUTORCC OFF)
    set(CMAKE_AUTOUIC OFF)
else()
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
endif()

# Output to build/bin
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── AI-only build mode: skip PDF/OpenSSL/QPDF dependencies ─────────────────
option(TORREADER_NO_PDFIUM "Skip PDFium + PDF dependencies (AI-only build)" OFF)

# ── OpenSSL (vcpkg) — PdfSigner verify + sign (CMS/PKCS12) ───────────────────
if(NOT TORREADER_NO_PDFIUM)
    find_package(OpenSSL REQUIRED)
else()
    # Stub so CMake resolves the target name in generator expressions
    add_library(OpenSSL::Crypto INTERFACE IMPORTED)
endif()

# ── Qt6 ──────────────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS
    Core Widgets Gui Concurrent PrintSupport OpenGL OpenGLWidgets Network Sql
)

# ── Viewer MOC (always needed — AUTOMOC may be OFF in AI-only mode) ───────────
if(WIN32)
qt6_wrap_cpp(VIEWER_MOC_SRCS
    src/viewer/DWFRenderer.h
    src/viewer/DWFViewer.h
    src/viewer/DWFMainWindow.h
)
else()
set(VIEWER_MOC_SRCS "")
endif()

# ── DWF Toolkit 7.7 (ODA pre-built) ──────────────────────────────────────────
if(WIN32)
set(DWF_DIR "${CMAKE_SOURCE_DIR}/third_party/dwf" CACHE PATH "DWF Toolkit directory")
set(DWF_INCLUDE_DIR "${DWF_DIR}/include" "${DWF_DIR}/include/dwf")

if(WIN32)
    set(DWF_WIN_LIB_DIR "${CMAKE_SOURCE_DIR}/AI/DWFToolkit-7.7-bin-ODA/lib/vc17_amd64md" CACHE PATH "DWF Toolkit Windows static libs")

    macro(_dwf_import_static _target _lib)
        add_library(${_target} STATIC IMPORTED GLOBAL)
        set_target_properties(${_target} PROPERTIES
            IMPORTED_LOCATION "${DWF_WIN_LIB_DIR}/${_lib}.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${DWF_INCLUDE_DIR}"
            INTERFACE_COMPILE_DEFINITIONS "DWFCORE_STATIC;DWFTK_STATIC;DWFTK_USE_DWFCORE_ZLIB"
        )
    endmacro()

    _dwf_import_static(dwf_toolkit DwfToolkit)
    _dwf_import_static(dwf_core DwfCore)
    _dwf_import_static(dwf_whiptk WhipTk)
    _dwf_import_static(dwf_w3dtk W3dTk)
    _dwf_import_static(dwf_zlib TD_Zlib)
    _dwf_import_static(dwf_alloc TD_Alloc)
    _dwf_import_static(dwf_jpeg TD_jpeg)

    # dwf_core first + repeated (circular dep core<->toolkit; MSVC static link order)
    set(DWF_LIBS dwf_core dwf_toolkit dwf_whiptk dwf_w3dtk dwf_zlib dwf_alloc dwf_jpeg dwf_core)
else()
    # Linux/macOS: static .a files
    set(DWF_LIB_DIR "${DWF_DIR}/lib")

    add_library(dwf_toolkit STATIC IMPORTED GLOBAL)
    set_target_properties(dwf_toolkit PROPERTIES
        IMPORTED_LOCATION "${DWF_LIB_DIR}/libDwfToolkit.a"
        INTERFACE_INCLUDE_DIRECTORIES "${DWF_INCLUDE_DIR}"
    )
    target_compile_definitions(dwf_toolkit INTERFACE
        DWFTK_STATIC DWFTK_BUILD_EXPAT EXPAT_COMPILED_FROM_DSP DWFCORE_STATIC
    )

    add_library(dwf_core STATIC IMPORTED GLOBAL)
    set_target_properties(dwf_core PROPERTIES
        IMPORTED_LOCATION "${DWF_LIB_DIR}/libDwfCore.a"
    )

    add_library(dwf_whiptk STATIC IMPORTED GLOBAL)
    set_target_properties(dwf_whiptk PROPERTIES
        IMPORTED_LOCATION "${DWF_LIB_DIR}/libWhipTk.a"
    )

    add_library(dwf_w3dtk STATIC IMPORTED GLOBAL)
    set_target_properties(dwf_w3dtk PROPERTIES
        IMPORTED_LOCATION "${DWF_LIB_DIR}/libW3dTk.a"
    )

    add_library(dwf_zlib STATIC IMPORTED GLOBAL)
    set_target_properties(dwf_zlib PROPERTIES
        IMPORTED_LOCATION "${DWF_LIB_DIR}/libTD_Zlib.a"
    )

    add_library(dwf_alloc STATIC IMPORTED GLOBAL)
    set_target_properties(dwf_alloc PROPERTIES
        IMPORTED_LOCATION "${DWF_LIB_DIR}/libTD_Alloc.a"
    )

    add_library(dwf_jpeg STATIC IMPORTED GLOBAL)
    set_target_properties(dwf_jpeg PROPERTIES
        IMPORTED_LOCATION "${DWF_LIB_DIR}/libTD_jpeg.a"
    )

    set(DWF_LIBS dwf_toolkit dwf_whiptk dwf_w3dtk dwf_core dwf_zlib dwf_alloc dwf_jpeg)
endif()
endif()

if(NOT WIN32)
set(DWF_DIR "")
set(DWF_INCLUDE_DIR "")
set(DWF_LIBS "")
endif()

# ── yaml-cpp (rule engine) ─────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG yaml-cpp-0.9.0
)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)

# ── PDFium (pdfium-binaries pre-built, optional) ─────────────────────────────
set(PDFIUM_DIR "${CMAKE_SOURCE_DIR}/third_party/pdfium" CACHE PATH "PDFium directory")
if(NOT TORREADER_NO_PDFIUM)
add_library(pdfium SHARED IMPORTED)
if(WIN32)
    set(_pdfium_dll "${PDFIUM_DIR}/bin/pdfium.dll")
    set(_pdfium_lib "${PDFIUM_DIR}/lib/pdfium.dll.lib")
    if(NOT EXISTS "${_pdfium_lib}")
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
        message(WARNING "PDFium không tìm thấy. Building without PDF support.")
    else()
        set_target_properties(pdfium PROPERTIES
            IMPORTED_LOCATION "${PDFIUM_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${PDFIUM_DIR}/include"
        )
    endif()
endif()
else()
    # Stub target so linker references don't break
    add_library(pdfium INTERFACE)
endif()
# ── QPDF (Apache 2.0, optional in AI-only mode) ──────────────────────────────
if(NOT TORREADER_NO_PDFIUM)
find_package(PkgConfig QUIET)
find_library(QPDF_LIB NAMES qpdf
    HINTS "${CMAKE_SOURCE_DIR}/third_party/qpdf/lib"
    NO_DEFAULT_PATH)
find_path(QPDF_INCLUDE NAMES qpdf/QPDF.hh
    HINTS "${CMAKE_SOURCE_DIR}/third_party/qpdf/include"
    NO_DEFAULT_PATH)
if(NOT QPDF_LIB OR NOT QPDF_INCLUDE)
    find_package(qpdf CONFIG QUIET)
    if(NOT qpdf_FOUND)
        message(FATAL_ERROR "QPDF không tìm thấy. Cài bằng: vcpkg install qpdf hoặc apt install libqpdf-dev")
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
else()
    # QPDF not needed for AI-only build — stub empty variable
    set(QPDF_TARGET "")
    set(qpdf_FOUND FALSE)
    # Create stub so CMake resolves target name references
    add_library(qpdf_stub INTERFACE)
    set(QPDF_TARGET qpdf_stub)
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

    # DWF AI engine
    src/dwf/DWFLoader.cpp

    # DWF Viewer
    src/viewer/DWFParse.cpp
    src/viewer/DWFRenderer.cpp
    src/viewer/DWFViewer.cpp
    src/viewer/DWFMainWindow.cpp
    ${VIEWER_MOC_SRCS}

    # Rule engine + core utilities
    src/core/Issue.cpp
    src/core/Reporter.cpp
    src/rules/RuleEngine.cpp
    src/rules/ConstructionRules.cpp
    src/rules/GeometryRules.cpp

    # AI Copilot (used by DWF viewer + harness in both build modes)
    src/ai/AICopilot.cpp
    src/ai/AICopilotPanel.cpp

    # Core PDF engine (disabled when building AI-only without PDFium)
    $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:
    src/core/PdfDocument.cpp
    src/core/PdfRenderer.cpp
    src/core/VectorLayer.cpp
    src/core/VectorGpuRenderer.cpp
    src/core/TileCacheFile.cpp
    src/core/ThumbnailRenderPool.cpp
    src/core/PdfEditor.cpp
    src/core/TextSearch.cpp
    src/core/UpdateChecker.cpp
    src/core/GoogleAuth.cpp
    src/core/Translator.cpp
    src/core/ForeignAnnotLayer.cpp

    # Annotations
    src/annotations/AnnotationLayer.cpp
    src/annotations/AnnotationTool.cpp
    src/annotations/AnnotationManager.cpp

    # Print
    src/core/PdfPrinter.cpp

    # Signer
    src/core/PdfSigner.cpp

    # UI
    src/ui/MainWindow.cpp
    src/ui/PdfView.cpp
    src/ui/PdfGpuView.cpp
    src/ui/ThumbnailPanel.cpp
    src/ui/SearchPanel.cpp
    src/ui/FindBar.cpp
    src/ui/ContinuousView.cpp
    src/ui/ToolBar.cpp
    src/ui/MergeDialog.cpp
    src/ui/SignDialog.cpp
    src/ui/PrintDialog.cpp
    src/ui/AboutDialog.cpp
    src/ui/NoteInputDialog.cpp
    src/ui/UpdateBanner.cpp
    src/ui/GateDialog.cpp
    src/ui/TranslationPopup.cpp
    src/ui/NotificationBar.cpp
    >
)

set(HEADERS
    # DWF AI engine
    src/dwf/DWFLoader.h

    # DWF Viewer
    src/viewer/DWFParse.h
    src/viewer/DWFRenderer.h
    src/viewer/DWFViewer.h
    src/viewer/DWFMainWindow.h

    # Rule engine + core utilities
    src/core/Issue.h
    src/core/Reporter.h
    src/rules/RuleEngine.h
    src/rules/ConstructionRules.h
    src/rules/GeometryRules.h
    src/ai/AICopilot.h
    src/ai/AICopilotPanel.h

    $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:
    src/core/PdfDocument.h
    src/core/PdfRenderer.h
    src/core/VectorLayer.h
    src/core/VectorGpuRenderer.h
    src/core/TileCacheFile.h
    src/core/PdfEditor.h
    src/core/PdfSigner.h
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
    src/ui/SignDialog.h
    src/ui/PrintDialog.h
    src/ui/AboutDialog.h
    src/ui/NoteInputDialog.h
    src/ui/UpdateBanner.h
    src/ui/GateDialog.h
    src/ui/TranslationPopup.h
    src/ui/NotificationBar.h
    >
)

if(FELIXPDF_ENABLE_OCR)
    list(APPEND SOURCES src/core/OcrEngine.cpp)
    list(APPEND HEADERS src/core/OcrEngine.h)
endif()

if(NOT WIN32)
    list(FILTER SOURCES EXCLUDE REGEX "(src/dwf/|src/viewer/DWF|src/core/Issue|src/core/Reporter|src/rules/|src/ai/AICopilot)")
    list(FILTER HEADERS EXCLUDE REGEX "(src/dwf/|src/viewer/DWF|src/core/Issue|src/core/Reporter|src/rules/|src/ai/AICopilot)")
endif()

# ── Executable ────────────────────────────────────────────────────────────────
if(WIN32)
    add_executable(TorReader WIN32
        ${SOURCES} ${HEADERS}
        $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:resources/resources.qrc>
        $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:resources/TorReader.rc>
    )
else()
    set(_EXTRA_SOURCES "")
    if(NOT TORREADER_NO_PDFIUM)
        list(APPEND _EXTRA_SOURCES resources/resources.qrc)
    endif()
    add_executable(TorReader ${SOURCES} ${HEADERS} ${_EXTRA_SOURCES})
endif()

target_compile_definitions(TorReader PRIVATE
    FELIXPDF_VERSION="${PROJECT_VERSION}"
    $<$<BOOL:${FELIXPDF_ENABLE_OCR}>:FELIXPDF_ENABLE_OCR>
    $<$<BOOL:${TORREADER_NO_PDFIUM}>:TORREADER_NO_PDFIUM>
)

target_include_directories(TorReader PRIVATE
    src src/rules
    ${DWF_INCLUDE_DIR}
    ${PDFIUM_DIR}/include
)

# DWF static libs compiled without -fPIC; link non-PIE (Linux only)
if(NOT WIN32)
    target_link_options(TorReader PRIVATE -no-pie)
endif()

target_link_libraries(TorReader PRIVATE
    yaml-cpp
    Qt6::Core
    Qt6::Widgets
    Qt6::Gui
    Qt6::Concurrent
    $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:Qt6::PrintSupport>
    $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:Qt6::OpenGL>
    $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:Qt6::OpenGLWidgets>
    Qt6::Network
    Qt6::Sql
    $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:pdfium>
    $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:${QPDF_TARGET}>
    $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:OpenSSL::Crypto>
    ${DWF_LIBS}
    $<$<NOT:$<BOOL:${WIN32}>>:lcms2>
    $<$<BOOL:${FELIXPDF_ENABLE_OCR}>:Tesseract::libtesseract>
    $<$<BOOL:${WIN32}>:rpcrt4>
    $<$<BOOL:${WIN32}>:psapi>
)

# DWF ODA libs compiled with /Zc:wchar_t- (wchar_t=unsigned short) → app uses native
# wchar_t; alias the expected wchar_t-mangled symbols to the ushort ones in the libs.
if(WIN32)
    set(DWF_WCHAR_ALIASES
        "?message@DWFException@DWFCore@@QEBAPEB_WXZ=?message@DWFException@DWFCore@@QEBAPEBGXZ"
        "??0DWFException@DWFCore@@IEAA@PEB_WPEBD0I@Z=??0DWFException@DWFCore@@IEAA@PEBGPEBD0I@Z"
        "??0DWFString@DWFCore@@QEAA@PEB_W@Z=??0DWFString@DWFCore@@QEAA@PEBG@Z"
        "??BDWFString@DWFCore@@QEBAPEB_WXZ=??BDWFString@DWFCore@@QEBAPEBGXZ"
        "?kzRole_Graphics2d@DWFXML@@2QEB_WEB=?kzRole_Graphics2d@DWFXML@@2QEBGEB"
        "?kzRole_Graphics2dOverlay@DWFXML@@2QEB_WEB=?kzRole_Graphics2dOverlay@DWFXML@@2QEBGEB"
        "?kzRole_Graphics2dMarkup@DWFXML@@2QEB_WEB=?kzRole_Graphics2dMarkup@DWFXML@@2QEBGEB"
    )
    foreach(_alias ${DWF_WCHAR_ALIASES})
        target_link_options(TorReader PRIVATE "/alternatename:${_alias}")
    endforeach()
endif()

# ── Test: merge bookmark remapping (optional, -DBUILD_TESTS=ON) ────────────────
if(BUILD_TESTS)
    add_executable(TestMerge tests/test_merge.cpp src/core/PdfEditor.cpp)
    target_include_directories(TestMerge PRIVATE src)
    target_link_libraries(TestMerge PRIVATE Qt6::Core ${QPDF_TARGET})
    set_target_properties(TestMerge PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

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
            COMMAND ${CMAKE_COMMAND} -E rm -f
                "$<TARGET_FILE_DIR:TorReader>/dxcompiler.dll"
                "$<TARGET_FILE_DIR:TorReader>/dxil.dll"
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

# ── Copy third-party license files to output (PDF builds only) ──────────────
if(NOT TORREADER_NO_PDFIUM)
    add_custom_command(TARGET TorReader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/dist/THIRD-PARTY-LICENSES.txt"
            "$<TARGET_FILE_DIR:TorReader>/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/dist/licenses"
            "$<TARGET_FILE_DIR:TorReader>/licenses"
        COMMENT "Copying third-party license files"
    )
endif()
