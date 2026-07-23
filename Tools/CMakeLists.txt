cmake_minimum_required(VERSION 3.25)
project(Tools)

# -----------------------------------------------------------------------------
# 2. External Dependencies (The "Nuget" Step)
# -----------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG v2.9.3
)
# Downloads Glaze and makes the 'glaze::glaze' target available
FetchContent_MakeAvailable(glaze)


FetchContent_Declare(
	magic_enum
	GIT_REPOSITORY https://github.com/Neargye/magic_enum.git
	GIT_TAG v0.9.5
)
FetchContent_MakeAvailable(magic_enum)


find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

# -----------------------------------------------------------------------------
# 3. Library Definition
# -----------------------------------------------------------------------------
# Optimization: Use STATIC with a dummy file ("Empty.cpp"). 
# Why? If we use INTERFACE (normal for header-only), CMake generates NO build 
# commands for this project, breaking IntelliSense in VS Code.
# STATIC forces CMake to "compile" this, creating a valid context for the headers.
add_library(ToolsLib STATIC 
    "Tools.hpp"
    "Json.hpp"
    "Timestamp.hpp"
    "SeqLock.hpp"
    "String.hpp"
    "Types.hpp"
    "Application.hpp"
    "ByteQueue.hpp"
    "Memory.hpp"
    "BlockingQueue.hpp"
    "Logger.hpp"
    "Telegram.hpp"
    "Empty.cpp"
)

# -----------------------------------------------------------------------------
# 4. Include & Link Configuration
# -----------------------------------------------------------------------------

# EXPLANATION:
# PUBLIC = "I use it, and you use it."
# 1. Adds this folder to the include path for 'Empty.cpp' (internal build).
# 2. Adds this folder to the include path for 'RivalServer' (consumer).
target_include_directories(ToolsLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# EXPLANATION:
# Links Glaze to ToolsLib. 
# PUBLIC means "If you link ToolsLib, you get Glaze automatically."
# This fixes your "glaze/glaze.hpp not found" error in RivalServer.
target_link_libraries(ToolsLib PUBLIC glaze::glaze magic_enum::magic_enum Threads::Threads CURL::libcurl)
# -----------------------------------------------------------------------------
# 5. Unit Tests
# -----------------------------------------------------------------------------
add_executable(ToolsTests "Main.cpp")

# Link privately because no one inherits dependencies from the Test runner.
target_link_libraries(ToolsTests PRIVATE ToolsLib)