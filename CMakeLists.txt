cmake_minimum_required(VERSION 3.16)
project(City LANGUAGES CXX)

find_package(Siv3D REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPENGL REQUIRED IMPORTED_TARGET gl)

file(GLOB_RECURSE CITY_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
add_executable(City Main.cpp ${CITY_SOURCES})
target_compile_features(City PRIVATE cxx_std_20)
target_link_libraries(City PRIVATE Siv3D::Siv3D PkgConfig::OPENGL)
target_compile_options(City PRIVATE -msse4.1 -include "${CMAKE_CURRENT_SOURCE_DIR}/stdafx.h")
set_target_properties(City PROPERTIES
	CXX_EXTENSIONS OFF
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/App"
)

# The Linux SDK keeps its engine assets outside the static library.
set(SIV3D_RESOURCE_DIR "" CACHE PATH "OpenSiv3D/Linux/App/resources directory")
if(SIV3D_RESOURCE_DIR)
	add_custom_command(TARGET City POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory
			"${SIV3D_RESOURCE_DIR}/engine"
			"$<TARGET_FILE_DIR:City>/resources/engine"
		VERBATIM
	)
endif()

include(cmake/LinuxTestSources.cmake)
add_executable(CityTests EXCLUDE_FROM_ALL ${CITY_TEST_SOURCES})
target_compile_features(CityTests PRIVATE cxx_std_20)
target_include_directories(CityTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(CityTests PRIVATE -msse4.1 -include "${CMAKE_CURRENT_SOURCE_DIR}/Test/stdafx.h")
target_link_libraries(CityTests PRIVATE Siv3D::Siv3D PkgConfig::OPENGL)
set_target_properties(CityTests PROPERTIES
	CXX_EXTENSIONS OFF
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Test/App"
)
if(SIV3D_RESOURCE_DIR)
	add_custom_command(TARGET CityTests POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory
			"${SIV3D_RESOURCE_DIR}/engine"
			"$<TARGET_FILE_DIR:CityTests>/resources/engine"
		VERBATIM
	)
endif()
