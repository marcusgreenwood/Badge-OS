file(GLOB PRBOOM_SRCS "${CMAKE_CURRENT_LIST_DIR}/*.c")
# OpenGL renderer not used on this target
list(REMOVE_ITEM PRBOOM_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/gl_main.c"
    "${CMAKE_CURRENT_LIST_DIR}/gl_texture.c")

idf_component_register(
    SRCS ${PRBOOM_SRCS}
    INCLUDE_DIRS "include" "."
    REQUIRES prboom-wad-tables)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -fno-strict-aliasing
    -Wno-error
    -Wno-char-subscripts
    -Wno-unused-value
    -Wno-parentheses
    -Wno-int-to-pointer-cast
    -Wno-pointer-sign
    -Wno-unused-but-set-variable
    -Wno-unused-variable
    -Wno-maybe-uninitialized
    -Wno-implicit-fallthrough
    -Wno-missing-field-initializers
    -Wno-format
    -Wno-format-truncation
    -Wno-old-style-declaration
    -Wno-discarded-qualifiers
    -Wno-implicit-function-declaration
    -Wno-array-bounds
    -Wno-stringop-overflow
    -Wno-misleading-indentation
    -Wno-duplicate-decl-specifier
    -Wno-type-limits
    -Wno-tautological-compare
    -Wno-empty-body
    -Wno-sequence-point
    -Wno-memset-transposed-args
    -Wno-enum-int-mismatch
    -Wno-sizeof-pointer-memaccess
    -Wno-sizeof-pointer-div
    -Wno-unused-but-set-parameter
    -Wno-dangling-else
    -Wno-shift-negative-value
    -Wno-bool-compare
    -Wno-logical-not-parentheses
    -Wno-unused-function
    -Wno-return-type)
