add_executable(test_network 
    test_network.cpp 
    ${CMAKE_SOURCE_DIR}/src/Network/Server.cpp 
    ${CMAKE_SOURCE_DIR}/src/Network/Client.cpp
)

target_link_libraries(test_network PRIVATE Boost::boost Boost::system)
target_include_directories(test_network PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Enregistre l'exécutable dans CTest
add_test(NAME NetworkIntegrationTest COMMAND test_network)
