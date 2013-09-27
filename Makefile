CFLAGS= -DMP4V2_USE_STATIC_LIB 
LFLAGS= -lshlwapi -lfaac -lmp4v2 -lx264 -static-libgcc -static-libstdc++ 

xfmp4.exe: src/xfmp4.cpp Makefile
	g++ -o xfmp4.exe -O2 src/xfmp4.cpp ${CFLAGS} ${LFLAGS}
