cmake_minimum_required (VERSION 3.25)
project ("HFT")

# Force C++23 globally for all targets
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -Wshadow and -Wconversion are lifesavers for math/logic bugs
add_compile_options(
	-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
	-fno-strict-aliasing
	-fPIC
	-Wno-missing-braces
	-Wodr
	$<$<CONFIG:Release>:-O3>
	$<$<CONFIG:Release>:-march=native>
	$<$<CONFIG:Debug>:-g>
)

# Just build these sub-folders. 
# Reordered Socket before Data so DataLib can depend on SocketLib
add_subdirectory ("Tools")
add_subdirectory ("Socket")
add_subdirectory ("Data")
add_subdirectory ("Execution")
add_subdirectory ("Provider")
add_subdirectory ("Strategy")