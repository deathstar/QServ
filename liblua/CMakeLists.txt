project(lua)

set(lua_srcs
	lapi.c 
	lcode.c
	ldebug.c
	ldo.c
	ldump.c
	lfunc.c
	lgc.c
	llex.c
	lmem.c
	lobject.c
	lopcodes.c
	lparser.c
	lstate.c
	lstring.c
	ltable.c
	ltm.c
	lundump.c
	lvm.c
	lzio.c
	lauxlib.c
	lbaselib.c
	ldblib.c
	liolib.c
	lmathlib.c
	loslib.c
	ltablib.c
	lstrlib.c
	loadlib.c
	linit.c
	libhttpd.c)


add_definitions(-DHAS_SOCKLEN_T)

include_directories(.)

add_library(lua STATIC ${lua_srcs})
