# macos specific packaging

set(CODESIGN_IDENTITY "" CACHE STRING "Codesign identity, e.g. 'Developer ID Application: Name (TEAMID)'")

# Build an .app
set(CMAKE_MACOSX_BUNDLE YES)

set(MAC_BUNDLE_NAME "${CMAKE_PROJECT_NAME}.app")
set(MAC_BUNDLE_CONTENTS "${MAC_BUNDLE_NAME}/Contents")
set(MAC_BUNDLE_RESOURCES "${MAC_BUNDLE_CONTENTS}/Resources")

install(TARGETS sunshine
    BUNDLE DESTINATION .
    COMPONENT Runtime)

set(SUNSHINE_ASSETS_DIR "${CMAKE_PROJECT_NAME}.app/Contents/Resources/assets")
set(SUNSHINE_ASSETS_DIR_DEF "../Resources/assets")

install(FILES "${APPLE_PLIST_FILE}"
        DESTINATION "${MAC_BUNDLE_CONTENTS}"
        COMPONENT Runtime)

install(FILES "${PROJECT_SOURCE_DIR}/src_assets/macos/assets/sunshine.icns"
        DESTINATION "${MAC_BUNDLE_RESOURCES}"
        COMPONENT Runtime)

# macOS-specific assets (apps.json, etc.)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/macos/assets/"
        DESTINATION "${MAC_BUNDLE_RESOURCES}/assets"
        COMPONENT Runtime
        PATTERN "Info.plist*" EXCLUDE
        PATTERN ".DS_Store" EXCLUDE
        PATTERN "._*" EXCLUDE)

# Pull in non-system dylibs for a self-contained .app
install(CODE "
    set(_app \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_NAME}.app\")

    message(STATUS \"Running fixup_bundle for: \${_app}\")
    include(BundleUtilities)
    set(BU_CHMOD_BUNDLE_ITEMS TRUE)
    fixup_bundle(\"\${_app}\" \"\" \"\")

    # Remove Finder/resource-fork metadata that breaks codesign.
    execute_process(COMMAND /usr/bin/xattr -rc \"\${_app}\")

    if(\"${CODESIGN_IDENTITY}\" STREQUAL \"\")
        message(WARNING \"CODESIGN_IDENTITY not set; removing all signatures\")
        execute_process(COMMAND /usr/bin/codesign
            --remove-signature --force --deep
            \"\${_app}\"
            RESULT_VARIABLE rc2
        )
        if(NOT rc2 EQUAL 0)
            message(FATAL_ERROR \"codesign failed for app\")
        endif()
        return()
    endif()

    # Sign anything inside Contents/Frameworks
    set(_fw_dir \"\${_app}/Contents/Frameworks\")
    if(EXISTS \"\${_fw_dir}\")
        file(GLOB_RECURSE _sign_items
            \"\${_fw_dir}/*.framework\"
            \"\${_fw_dir}/*.dylib\"
        )
        foreach(item IN LISTS _sign_items)
            execute_process(COMMAND /usr/bin/codesign
                --force --timestamp --options runtime
                --sign \"${CODESIGN_IDENTITY}\"
                \"\${item}\"
                RESULT_VARIABLE rc
            )
            if(NOT rc EQUAL 0)
                message(FATAL_ERROR \"codesign failed for \${item}\")
            endif()
        endforeach()
    endif()

    # Sign the app last
    execute_process(COMMAND /usr/bin/codesign
        --force --timestamp --options runtime
        --sign \"${CODESIGN_IDENTITY}\"
        \"\${_app}\"
        RESULT_VARIABLE rc2
    )
    if(NOT rc2 EQUAL 0)
        message(FATAL_ERROR \"codesign failed for app\")
    endif()

    # Verify
    execute_process(COMMAND /usr/bin/codesign --verify --deep --strict --verbose=2 \"\${_app}\"
        RESULT_VARIABLE rc3
    )
    if(NOT rc3 EQUAL 0)
        message(FATAL_ERROR \"codesign verification failed\")
    endif()
" COMPONENT Runtime)

set(CPACK_GENERATOR "DragNDrop")
set(CPACK_BUNDLE_NAME "${CMAKE_PROJECT_NAME}")
set(CPACK_BUNDLE_PLIST "${APPLE_PLIST_FILE}")
set(CPACK_BUNDLE_ICON "${PROJECT_SOURCE_DIR}/src_assets/macos/assets/sunshine.icns")
set(CPACK_PACKAGING_INSTALL_PREFIX "/")
set(CPACK_DMG_BACKGROUND_IMAGE "${PROJECT_SOURCE_DIR}/gh-pages-template/assets/img/banners/AdobeStock_305732536.jpeg")
set(CPACK_DMG_DS_STORE "${PROJECT_SOURCE_DIR}/src_assets/macos/assets/dot_DS_Store")
