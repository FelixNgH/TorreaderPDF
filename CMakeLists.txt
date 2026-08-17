cmake_minimum_required(VERSION 3.25)
project(TorReader VERSION 2.4.0 LANGUAGES CXX)

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

# Khong co co /utf-8 thi MSVC doc nguon theo codepage ANSI, moi chuoi UTF-8 trong ma se thanh mojibake, con GCC tren Linux van dung nen loi CHI HIEN TREN WINDOWS.
if(MSVC)
    add_compile_options(/utf-8)
endif()

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

# ── ZLIB (giai nen .tar.gz goi OCR) ──────────────────────────────────────────
find_package(ZLIB REQUIRED)

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
# Khong tim thay Tesseract => FELIXPDF_ENABLE_OCR=OFF, app VAN build duoc
# (OcrEngine la stub: available()=false kem ly do, UI hien ly do do).
#   Windows: vcpkg da cai tesseract55 -> find_package(Tesseract CONFIG)
#   Linux  : goi he thong hoac PKG_CONFIG_PATH tro toi bo cai rieng.
#            Khi cau hinh: dat PKG_CONFIG_SYSROOT_DIR + PKG_CONFIG_PATH.
#            KHONG hardcode duong dan — may khac co tesseract he thong van chay.
set(FELIXPDF_ENABLE_OCR OFF)
set(TESSERACT_TARGET "")

find_package(PkgConfig QUIET)

if(WIN32)
    find_package(Tesseract CONFIG QUIET)
    if(Tesseract_FOUND)
        set(FELIXPDF_ENABLE_OCR ON)
        set(TESSERACT_TARGET Tesseract::libtesseract)
    endif()
elseif(PkgConfig_FOUND)
    pkg_check_modules(TESSERACT_PC QUIET IMPORTED_TARGET tesseract lept)
    if(TESSERACT_PC_FOUND)
        set(FELIXPDF_ENABLE_OCR ON)
        set(TESSERACT_TARGET PkgConfig::TESSERACT_PC)
    endif()
endif()

if(NOT FELIXPDF_ENABLE_OCR)
    message(WARNING "Tesseract khong tim thay — OCR bi tat. App van build duoc.")
endif()

# ── Sources ───────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp

    # Core PDF engine (disabled when building AI-only without PDFium)
    $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:
    src/core/PdfDocument.cpp
    src/core/PdfRenderer.cpp
    src/core/TileCacheFile.cpp
    src/core/ThumbnailRenderPool.cpp
    src/core/PdfEditor.cpp
    src/core/TextSearch.cpp
    src/core/TextSelection.cpp
    src/core/UpdateChecker.cpp
    src/core/GoogleAuth.cpp
    src/core/Translator.cpp
    src/core/VectorLayer.cpp
    src/core/VectorGpuRenderer.cpp
    src/core/ForeignAnnotLayer.cpp
    src/core/PdfLinks.cpp
    src/core/PageCache.cpp

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
    src/ui/OcrPanel.cpp
    src/ui/FindBar.cpp
    src/ui/ContinuousView.cpp
    src/ui/ToolBar.cpp
    src/ui/UiProbe.cpp
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
    $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:
    src/core/PdfDocument.h
    src/core/PdfRenderer.h
    src/core/TileCacheFile.h
    src/core/PdfEditor.h
    src/core/PdfSigner.h
    src/core/PdfPrinter.h
    src/core/TextSearch.h
    src/core/TextSelection.h
    src/core/UpdateChecker.h
    src/core/GoogleAuth.h
    src/core/Translator.h
    src/core/VectorLayer.h
    src/core/VectorGpuRenderer.h
    src/core/OwnAnnotHideGuard.h
    src/core/ForeignAnnotLayer.h
    src/core/PdfLinks.h
    src/annotations/AnnotationLayer.h
    src/annotations/AnnotationTool.h
    src/annotations/AnnotationTypes.h
    src/annotations/AnnotationManager.h
    src/ui/MainWindow.h
    src/ui/ThemeTokens.h
    src/ui/PdfView.h
    src/ui/PdfGpuView.h
    src/ui/ThumbnailPanel.h
    src/ui/SearchPanel.h
    src/ui/OcrPanel.h
    src/ui/ContinuousView.h
    src/ui/ToolBar.h
    src/ui/UiProbe.h
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

# OcrEngine/OcrTextLayer luon compile de dialog lien ket duoc. Tesseract code
# trong OcrEngine.cpp nam sau #ifdef FELIXPDF_ENABLE_OCR; khong co thi
# available()=false + ly do. Chi trong build PDF (can PDFium).
if(NOT TORREADER_NO_PDFIUM)
    list(APPEND SOURCES src/core/OcrEngine.cpp src/core/OcrTextLayer.cpp)
    list(APPEND HEADERS src/core/OcrEngine.h src/core/OcrTextLayer.h)
endif()

# ── Windows version resource (single source of truth = PROJECT_VERSION) ───────
configure_file(
    resources/torreader_version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/torreader_version.h
    @ONLY
)

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
    src
    ${CMAKE_CURRENT_BINARY_DIR}
    ${PDFIUM_DIR}/include
)

target_link_libraries(TorReader PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Gui
    Qt6::Concurrent
    $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:Qt6::PrintSupport>
    $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:Qt6::OpenGL>
    $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:Qt6::OpenGLWidgets>
    Qt6::Network
    Qt6::Sql
    ZLIB::ZLIB
    $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:pdfium>
    $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:${QPDF_TARGET}>
    $<$<NOT:$<BOOL:${TORREADER_NO_PDFIUM}>>:OpenSSL::Crypto>
    $<$<BOOL:${FELIXPDF_ENABLE_OCR}>:${TESSERACT_TARGET}>
    $<$<BOOL:${WIN32}>:rpcrt4>
    $<$<BOOL:${WIN32}>:psapi>
)

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
# POST_BUILD + $<TARGET_FILE_DIR:TorReader> de dich giai dung theo TUNG cau
# hinh. MSVC da cau hinh: exe that o <out>/Release/, file(COPY) luc cau hinh
# chi vao <out>/ => tessdata/licenses nam sai cho, OCR khong thay goi nguon.
# Tren Linux (Ninja mot cau hinh) hai cho trung nhau nen loi cu bi bo sot.
if(NOT TORREADER_NO_PDFIUM)
    add_custom_command(TARGET TorReader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/dist/THIRD-PARTY-LICENSES.txt"
            $<TARGET_FILE_DIR:TorReader>
        COMMENT "Copying THIRD-PARTY-LICENSES.txt" VERBATIM)
    add_custom_command(TARGET TorReader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/dist/licenses"
            "$<TARGET_FILE_DIR:TorReader>/licenses"
        COMMENT "Copying dist/licenses" VERBATIM)
    # Tesseract traineddata: chép eng + vie vào thư mục cạnh file chạy
    # (spec OCR 3a muc 4). OcrEngine::tessdataDir() tim tai appDir/tessdata.
    if(EXISTS "${CMAKE_SOURCE_DIR}/resources/tessdata")
        add_custom_command(TARGET TorReader POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/resources/tessdata"
                "$<TARGET_FILE_DIR:TorReader>/tessdata"
            COMMENT "Copying resources/tessdata" VERBATIM)
    endif()
endif()
