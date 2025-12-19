add_library(slopnet STATIC "slopnet.c" "slopnet_fetch.c")
target_include_directories(slopnet PUBLIC "../include")
target_link_libraries(slopnet PRIVATE cute)
