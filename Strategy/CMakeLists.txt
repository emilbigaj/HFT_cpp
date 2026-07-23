project(Strategy)

# --- A. The Library (SocketLib) ---
add_library(StrategyLib STATIC 
    "Strategy.hpp"
    "Scenario.hpp"
    "Empty.cpp"
)

# Link ToolsLib because Socket uses it
target_link_libraries(StrategyLib PUBLIC ToolsLib ExecutionLib DataLib SocketLib ProviderLib)

# Allow others to see headers in this folder
target_include_directories(StrategyLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# --- B. The Test App (ServerTests.exe) ---
add_executable(Strategy "Main.cpp")

# Links SocketLib (which automatically pulls in ToolsLib too)
target_link_libraries(Strategy PRIVATE StrategyLib)