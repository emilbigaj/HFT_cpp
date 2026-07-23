project(Data)

add_library(DataLib STATIC 
    "Tick.hpp"
    "MarketByPrice.hpp"
    "Instrument.hpp"
    "Empty.cpp"
)

# anyone that link DataLib will be able to include headers from this folder
target_include_directories(DataLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(DataLib PRIVATE ${CMAKE_SOURCE_DIR}/Tools)

# DataLib depends on ToolsLib and SocketLib
target_link_libraries(DataLib PUBLIC ToolsLib SocketLib)

add_executable(DataTests "Main.cpp")
target_link_libraries(DataTests PRIVATE DataLib)