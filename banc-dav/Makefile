# Banc natif du serveur WebDAV (dav.cpp, carte.cpp du port) : voir banc.cpp
CXXFLAGS = -std=gnu++17 -g -O1 -Wall -I stubs -I ..
banc-dav: banc.cpp ../dav.cpp ../carte.cpp ../dav.h ../carte.h
	$(CXX) $(CXXFLAGS) -o $@ banc.cpp ../dav.cpp ../carte.cpp -lpthread
clean:
	rm -f banc-dav
