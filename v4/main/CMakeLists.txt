idf_component_register(
    SRCS
        "main.c"
        "arc_phy.c"
        "rx_recal.c"
        "rf.c"
        "phy_rx_lab.c"
        "direct_gain_v3.c"
        "analog_video_detect.c"
        "menu_raster.c"
        "video.c"
        "video_calibration.c"
        "video_console.c"
        "video_control.c"
        "video_diagnostics.c"
        "video_dco.c"
        "video_drift.c"
        "video_recorder.c"
        "video_sampling.c"
        "video_gain.c"
        "video_idle.c"
        "video_lab.c"
        "video_measure.c"
        "video_menu.c"
        "video_settings.c"
        "video_transport.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_phy
        nvs_flash
        esp_event
        esp_netif
        esp_driver_parlio
        esp_driver_gpio
        esp_driver_bitscrambler
        esp_driver_usb_serial_jtag
        vfs
        esp_rom
        esp_mm
        esp_timer
        freertos
        heap
        hal
        esp_hw_support
        esp_driver_gptimer
        esp_driver_tsens
)

include("${PROJECT_DIR}/component.cmake")

# Recovered MMIO overrides are valid only for this exact C5 archive. Unknown
# library versions retain observation but refuse all lab writes/I2C helpers.
set(C5_PHY_ARCHIVE "$ENV{IDF_PATH}/components/esp_phy/lib/esp32c5/libphy.a")
if(EXISTS "${C5_PHY_ARCHIVE}")
    file(SHA256 "${C5_PHY_ARCHIVE}" C5_PHY_SHA256)
    if(C5_PHY_SHA256 STREQUAL "dbf33c418c8d408d4005c849d12a1432deea82e2e5e57de3c8ddf914d104fffb")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE C5VRX_PHY_RX_LAB_PINNED=1)
        # rx_recal.c keeps the vendor calibration's internal reference tunes on
        # the receive frequency (ESPARGOS esp-sdr route, verified on this pin).
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=phy_chip_set_chan_ana")
    else()
        message(WARNING "Unverified C5 PHY: RX lab writes disabled (${C5_PHY_SHA256})")
    endif()
endif()
