add_library(slopnet STATIC "slopnet.c")
target_include_directories(slopnet PUBLIC "../include")
target_link_libraries(slopnet PRIVATE cute)
