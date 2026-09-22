if(NOT DEFINED LANDROP_DEPLOY_DIRECTORY OR LANDROP_DEPLOY_DIRECTORY STREQUAL "")
    message(FATAL_ERROR "LANDROP_DEPLOY_DIRECTORY was not provided")
endif()

set(_landrop_required_runtime_files
    "Qt6Core.dll"
    "Qt6Gui.dll"
    "Qt6Network.dll"
    "Qt6Qml.dll"
    "Qt6Quick.dll"
    "Qt6Sql.dll"
    "Qt6Multimedia.dll"
    "platforms/qwindows.dll"
    "sqldrivers/qsqlite.dll"
)

if(LANDROP_MINGW)
    list(APPEND _landrop_required_runtime_files
        "libgcc_s_seh-1.dll"
        "libstdc++-6.dll"
        "libwinpthread-1.dll"
    )
endif()

set(_landrop_missing_runtime_files)
foreach(_landrop_runtime_file IN LISTS _landrop_required_runtime_files)
    if(NOT EXISTS "${LANDROP_DEPLOY_DIRECTORY}/${_landrop_runtime_file}")
        list(APPEND _landrop_missing_runtime_files "${_landrop_runtime_file}")
    endif()
endforeach()

if(_landrop_missing_runtime_files)
    list(JOIN _landrop_missing_runtime_files ", " _landrop_missing_text)
    message(FATAL_ERROR
        "Windows runtime deployment is incomplete in ${LANDROP_DEPLOY_DIRECTORY}. "
        "Missing: ${_landrop_missing_text}")
endif()

message(STATUS "Windows runtime deployment verified: ${LANDROP_DEPLOY_DIRECTORY}")
