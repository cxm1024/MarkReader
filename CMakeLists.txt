cmake_minimum_required(VERSION 3.20)
project(MarkReader LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Optimization & warning flags
if (MINGW OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-deprecated-declarations)
endif()

# ---------------------------------------------------------------------------
# Third-party: MD4C (Markdown + GFM + Math parser)
# ---------------------------------------------------------------------------
add_library(md4c STATIC
    third_party/md4c/src/md4c.c
    third_party/md4c/src/entity.c
)
target_include_directories(md4c PUBLIC third_party/md4c/src)

# ---------------------------------------------------------------------------
# Third-party: TinyXML2 (used by MicroTeX resource parser)
# ---------------------------------------------------------------------------
add_library(tinyxml2 STATIC
    third_party/tinyxml2/tinyxml2.cpp
)
target_include_directories(tinyxml2 PUBLIC third_party/tinyxml2)

# ---------------------------------------------------------------------------
# Third-party: MicroTeX (LaTeX math layout engine, Direct2D backend supplied by us)
# ---------------------------------------------------------------------------
add_library(microtex STATIC
    third_party/MicroTeX/src/atom/atom_basic.cpp
    third_party/MicroTeX/src/atom/atom_char.cpp
    third_party/MicroTeX/src/atom/atom_impl.cpp
    third_party/MicroTeX/src/atom/atom_matrix.cpp
    third_party/MicroTeX/src/atom/atom_row.cpp
    third_party/MicroTeX/src/atom/atom_space.cpp
    third_party/MicroTeX/src/atom/colors_def.cpp
    third_party/MicroTeX/src/atom/unit_conversion.cpp
    third_party/MicroTeX/src/box/box.cpp
    third_party/MicroTeX/src/box/box_factory.cpp
    third_party/MicroTeX/src/box/box_group.cpp
    third_party/MicroTeX/src/box/box_single.cpp
    third_party/MicroTeX/src/core/core.cpp
    third_party/MicroTeX/src/core/formula.cpp
    third_party/MicroTeX/src/core/formula_def.cpp
    third_party/MicroTeX/src/core/glue.cpp
    third_party/MicroTeX/src/core/localized_num.cpp
    third_party/MicroTeX/src/core/macro.cpp
    third_party/MicroTeX/src/core/macro_def.cpp
    third_party/MicroTeX/src/core/macro_impl.cpp
    third_party/MicroTeX/src/core/parser.cpp
    third_party/MicroTeX/src/fonts/alphabet.cpp
    third_party/MicroTeX/src/fonts/font_basic.cpp
    third_party/MicroTeX/src/fonts/font_info.cpp
    third_party/MicroTeX/src/fonts/fonts.cpp
    third_party/MicroTeX/src/utils/string_utils.cpp
    third_party/MicroTeX/src/utils/utf.cpp
    third_party/MicroTeX/src/utils/utils.cpp
    third_party/MicroTeX/src/res/builtin/formula_mappings.res.cpp
    third_party/MicroTeX/src/res/builtin/symbol_mapping.res.cpp
    third_party/MicroTeX/src/res/builtin/tex_param.res.cpp
    third_party/MicroTeX/src/res/builtin/tex_symbols.res.cpp
    third_party/MicroTeX/src/res/font/bi10.def.cpp
    third_party/MicroTeX/src/res/font/bx10.def.cpp
    third_party/MicroTeX/src/res/font/cmbsy10.def.cpp
    third_party/MicroTeX/src/res/font/cmbx10.def.cpp
    third_party/MicroTeX/src/res/font/cmbxti10.def.cpp
    third_party/MicroTeX/src/res/font/cmex10.def.cpp
    third_party/MicroTeX/src/res/font/cmmi10.def.cpp
    third_party/MicroTeX/src/res/font/cmmi10_unchanged.def.cpp
    third_party/MicroTeX/src/res/font/cmmib10.def.cpp
    third_party/MicroTeX/src/res/font/cmmib10_unchanged.def.cpp
    third_party/MicroTeX/src/res/font/cmr10.def.cpp
    third_party/MicroTeX/src/res/font/cmss10.def.cpp
    third_party/MicroTeX/src/res/font/cmssbx10.def.cpp
    third_party/MicroTeX/src/res/font/cmssi10.def.cpp
    third_party/MicroTeX/src/res/font/cmsy10.def.cpp
    third_party/MicroTeX/src/res/font/cmti10.def.cpp
    third_party/MicroTeX/src/res/font/cmti10_unchanged.def.cpp
    third_party/MicroTeX/src/res/font/cmtt10.def.cpp
    third_party/MicroTeX/src/res/font/dsrom10.def.cpp
    third_party/MicroTeX/src/res/font/eufb10.def.cpp
    third_party/MicroTeX/src/res/font/eufm10.def.cpp
    third_party/MicroTeX/src/res/font/i10.def.cpp
    third_party/MicroTeX/src/res/font/moustache.def.cpp
    third_party/MicroTeX/src/res/font/msam10.def.cpp
    third_party/MicroTeX/src/res/font/msbm10.def.cpp
    third_party/MicroTeX/src/res/font/r10.def.cpp
    third_party/MicroTeX/src/res/font/r10_unchanged.def.cpp
    third_party/MicroTeX/src/res/font/rsfs10.def.cpp
    third_party/MicroTeX/src/res/font/sb10.def.cpp
    third_party/MicroTeX/src/res/font/sbi10.def.cpp
    third_party/MicroTeX/src/res/font/si10.def.cpp
    third_party/MicroTeX/src/res/font/special.def.cpp
    third_party/MicroTeX/src/res/font/ss10.def.cpp
    third_party/MicroTeX/src/res/font/stmary10.def.cpp
    third_party/MicroTeX/src/res/font/tt10.def.cpp
    third_party/MicroTeX/src/res/parser/font_parser.cpp
    third_party/MicroTeX/src/res/parser/formula_parser.cpp
    third_party/MicroTeX/src/res/reg/builtin_font_reg.cpp
    third_party/MicroTeX/src/res/reg/builtin_syms_reg.cpp
    third_party/MicroTeX/src/res/sym/amsfonts.def.cpp
    third_party/MicroTeX/src/res/sym/amssymb.def.cpp
    third_party/MicroTeX/src/res/sym/base.def.cpp
    third_party/MicroTeX/src/res/sym/stmaryrd.def.cpp
    third_party/MicroTeX/src/res/sym/symspecial.def.cpp
    third_party/MicroTeX/src/latex.cpp
    third_party/MicroTeX/src/render.cpp
)
target_include_directories(microtex PUBLIC third_party/MicroTeX/src)
target_link_libraries(microtex PRIVATE tinyxml2)
target_compile_definitions(microtex PUBLIC NOMINMAX)
target_compile_options(microtex PRIVATE -w)

# ---------------------------------------------------------------------------
# Core Library (Markdown AST, Syntax Highlighter, File Watcher)
# ---------------------------------------------------------------------------
add_library(markreader_core STATIC
    src/core/types.cpp
    src/core/syntax_hl.cpp
    src/core/markdown_doc.cpp
    src/core/file_watcher.cpp
    src/core/sample_doc.cpp
)
target_include_directories(markreader_core PUBLIC src)
target_link_libraries(markreader_core PUBLIC md4c)
target_compile_definitions(markreader_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# ---------------------------------------------------------------------------
# Main GUI Executable
# ---------------------------------------------------------------------------
add_executable(MarkReader WIN32
    src/main.cpp
    src/math/d2d_tex_backend.cpp
    src/render/d2d_context.cpp
    src/render/image_loader.cpp
    src/render/text_renderer.cpp
    src/render/doc_layout.cpp
    src/ui/theme.cpp
    src/ui/source_editor.cpp
    src/ui/main_window.cpp
    resources/app.rc
)
target_include_directories(MarkReader PRIVATE src)
target_link_libraries(MarkReader PRIVATE
    markreader_core
    microtex
    d3d11
    dxgi
    d2d1
    dwrite
    windowscodecs
    dwmapi
    imm32
    winhttp
    ole32
    shell32
    shlwapi
    comdlg32
    uuid
)
target_compile_definitions(MarkReader PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if (MINGW)
    target_link_options(MarkReader PRIVATE -municode -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
endif()

# Copy MicroTeX fonts & XML resources next to executable
add_custom_command(TARGET MarkReader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/MicroTeX/res"
        "$<TARGET_FILE_DIR:MarkReader>/res"
    COMMENT "Copying MicroTeX resources to output directory"
)

# ---------------------------------------------------------------------------
# Unit Tests
# ---------------------------------------------------------------------------
enable_testing()
add_executable(markreader_tests
    tests/test_markdown.cpp
    src/ui/source_editor.cpp
    src/ui/theme.cpp
    src/render/text_renderer.cpp
    src/render/image_loader.cpp
    src/render/d2d_context.cpp
    src/math/d2d_tex_backend.cpp
)
target_include_directories(markreader_tests PRIVATE src)
target_link_libraries(markreader_tests PRIVATE
    markreader_core
    microtex
    d3d11
    dxgi
    d2d1
    dwrite
    windowscodecs
    winhttp
    ole32
    shlwapi
    uuid
)
if (MINGW)
    target_link_options(markreader_tests PRIVATE -static-libgcc -static-libstdc++)
endif()
add_test(NAME MarkReaderUnitTests COMMAND markreader_tests)

# ---------------------------------------------------------------------------
# Installer Targets (mr_packer, Uninstall, MarkReaderSetup)
# ---------------------------------------------------------------------------
add_executable(mr_packer
    installer/packer.cpp
)
target_include_directories(mr_packer PRIVATE installer)
target_link_libraries(mr_packer PRIVATE cabinet)
target_compile_definitions(mr_packer PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000000)
if (MINGW)
    target_link_options(mr_packer PRIVATE -municode -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
endif()

add_executable(Uninstall WIN32
    installer/uninstall.cpp
    installer/uninstall.rc
)
target_include_directories(Uninstall PRIVATE installer)
target_link_libraries(Uninstall PRIVATE ole32 shell32 uuid)
target_compile_definitions(Uninstall PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000000)
if (MINGW)
    target_link_options(Uninstall PRIVATE -municode -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
endif()

set(PAYLOAD_BIN_PATH "${CMAKE_CURRENT_BINARY_DIR}/payload.bin")
set(SETUP_MANIFEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/installer/setup.manifest")
set(APP_ICO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/installer/setup.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/setup.rc"
    @ONLY
)

file(GLOB_RECURSE MICROTEX_RES_FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/MicroTeX/res/*")
add_custom_command(
    OUTPUT "${PAYLOAD_BIN_PATH}"
    COMMAND mr_packer "${PAYLOAD_BIN_PATH}" "$<TARGET_FILE:MarkReader>" "$<TARGET_FILE:Uninstall>" "${CMAKE_CURRENT_SOURCE_DIR}/third_party/MicroTeX/res"
    DEPENDS MarkReader Uninstall mr_packer ${MICROTEX_RES_FILES}
    COMMENT "Compressing MarkReader installer payload (LZMS)"
    VERBATIM
)

set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/setup.rc" PROPERTIES
    OBJECT_DEPENDS "${PAYLOAD_BIN_PATH}"
)

add_executable(MarkReaderSetup WIN32
    installer/setup.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/setup.rc"
)
target_include_directories(MarkReaderSetup PRIVATE installer)
target_link_libraries(MarkReaderSetup PRIVATE cabinet comctl32 dwmapi ole32 shell32 uuid)
target_compile_definitions(MarkReaderSetup PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000000)
if (MINGW)
    target_link_options(MarkReaderSetup PRIVATE -municode -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
endif()

