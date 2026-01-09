idf_component_register(SRCS "dma2d_hal_custom.c"
                       PRIV_REQUIRES hal)

target_link_options(${COMPONENT_LIB} INTERFACE
    "LINKER:--wrap=dma2d_hal_tx_reset_channel"
)
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--whole-archive $<TARGET_FILE:${COMPONENT_LIB}> -Wl,--no-whole-archive"
)
