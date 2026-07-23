project(Provider)

# --- A. The Library (SocketLib) ---
add_library(ProviderLib STATIC 
    "Allocate.hpp"
    "Context.hpp"
    "Position.hpp"
    "Client.hpp"
    "RiskLayer.hpp"
    "Server.hpp"
    "AlertManager.hpp"
    "Empty.cpp"
)

# Link ToolsLib because Socket uses it
target_link_libraries(ProviderLib PUBLIC ToolsLib ExecutionLib DataLib SocketLib)

# Allow others to see headers in this folder
target_include_directories(ProviderLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# --- B. The Test App (ServerTests.exe) ---
add_executable(ProviderTests "Main.cpp")

# Links SocketLib (which automatically pulls in ToolsLib too)
target_link_libraries(ProviderTests PRIVATE ProviderLib)