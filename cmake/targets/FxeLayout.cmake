# fxe_layout — Facebook Yoga flexbox solver wrapper used by fxe_js's
# `Layout` binding. Static library; depends on `yoga::yogacore` from vcpkg
# (FetchContent fallback handled in cmake/deps.cmake).
file(GLOB _fxe_layout_sources CONFIGURE_DEPENDS src/layout/*.cpp)
add_library(fxe_layout STATIC ${_fxe_layout_sources})
add_library(fxe::layout ALIAS fxe_layout)
target_include_directories(fxe_layout PUBLIC include src)
target_compile_features(fxe_layout PUBLIC cxx_std_20)
if(TARGET yoga::yogacore)
    target_link_libraries(fxe_layout PUBLIC yoga::yogacore)
elseif(TARGET yogacore)
    target_link_libraries(fxe_layout PUBLIC yogacore)
else()
    message(FATAL_ERROR "yoga (yogacore) target not found; install via vcpkg manifest or enable FXE_FETCH_DEPS")
endif()
target_link_libraries(fxe_layout PUBLIC fxe_core)
