set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

idf_component_register(
    SRCS
        "main.c"
        "panel_bus.c"
        "storage.c"
        "buttons.c"
        "net.c"
        "provisioning.c"
        "${ROOT}/core/app.c"
        "${ROOT}/core/json.c"
        "${ROOT}/core/screen.c"
        "${ROOT}/core/fonts/np_fonts.c"
        "${ROOT}/third_party/qrcodegen/qrcodegen.c"
        "${ROOT}/drivers/el133uf1/el133uf1.c"
    INCLUDE_DIRS
        "."
        "${ROOT}/core"
        "${ROOT}/third_party/qrcodegen"
        "${ROOT}/drivers/el133uf1"
)
