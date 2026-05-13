set(src_dirs
            LCD
            TOUCH)

set(include_dirs
            LCD
            TOUCH)

set(requires
            driver
            esp_lcd
            esp_common
            log
            esp_adc)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)