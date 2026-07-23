project(Socket)

# --- A. The Library (SocketLib) ---
add_library(SocketLib STATIC 
    "Socket.hpp"
    "Protocol.hpp"
    "SharedArray.hpp"
    "LetterBox.hpp"
    "SharedMemory.hpp"
    "Empty.cpp"
)


# Link ToolsLib because Socket uses it
target_link_libraries(SocketLib PUBLIC ToolsLib)

# Allow others to see headers in this folder
target_include_directories(SocketLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# --- B. The Test App (SocketTests.exe) ---
add_executable(SocketTests "Main.cpp")

# Links SocketLib (which automatically pulls in ToolsLib too)
target_link_libraries(SocketTests PRIVATE SocketLib)