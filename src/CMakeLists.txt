add_library(slopnet STATIC
	"slopnet.c"
	"slopnet_fetch.c"
	"slopnet_transport.c"
	"slopnet_oauth.c"
)
target_include_directories(slopnet PUBLIC "../include")
target_link_libraries(slopnet PRIVATE cute)
if (EMSCRIPTEN)
	target_sources(slopnet PRIVATE "reliable/reliable.c" "slopnet_webtransport.c")
	target_link_options(slopnet PUBLIC
		"--js-library=${CMAKE_CURRENT_LIST_DIR}/slopnet_oauth.js"
		"--js-library=${CMAKE_CURRENT_LIST_DIR}/slopnet_transport.js"
		"-sFETCH"
	)
endif ()
