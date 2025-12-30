add_executable(slopnet-demo "main.c")
target_link_libraries(slopnet-demo PRIVATE cute slopnet)

if (EMSCRIPTEN)
	set_target_properties(slopnet-demo PROPERTIES OUTPUT_NAME "slopnet-demo" SUFFIX ".html")
	target_compile_options(slopnet-demo PRIVATE -fno-rtti -fno-exceptions -gsplit-dwarf)
	target_link_options(slopnet-demo PRIVATE
		-sASYNCIFY=1
		-sMALLOC=emmalloc-debug
		-gseparate-dwarf
		-lidbfs.js
	)
endif()
