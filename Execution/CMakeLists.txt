project(Execution)

# --- A. The Library (ExecutionLib) ---
add_library(ExecutionLib STATIC
    "Empty.cpp"
    "Order.hpp"
    "OrderIdAllocator.hpp"
    "RateLimit.hpp"
)

# Consumers of ExecutionLib (like ExecutionTests) will automatically 
# link to ToolsLib and DataLib as well.
target_link_libraries(ExecutionLib PUBLIC ToolsLib DataLib)

# any target that links to ExecutionLib will also see headers in this folder
target_include_directories(ExecutionLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# independent test app
add_executable(ExecutionTests "Main.cpp")
target_link_libraries(ExecutionTests PRIVATE ExecutionLib)