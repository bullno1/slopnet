add_library(slopnet STATIC "slopnet.c" "slopnet_fetch.c" "slopnet_transport.c" "slopnet_oauth.c")
target_include_directories(slopnet PUBLIC "../include")
target_link_libraries(slopnet PRIVATE cute)
if (EMSCRIPTEN)
	target_link_options(slopnet INTERFACE
		-sFILESYSTEM=1
		-sASYNCIFY=1
		-sASYNCIFY_STACK_SIZE=2040000
	)
endif ()
