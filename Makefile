CXX      = clang++
CXXFLAGS = -g -Wall -fPIC -std=c++11 -stdlib=libc++
LDFLAGS  = -lpthread
SOFLAGS  = -shared

LIBOCTET_STATIC = liboctet.a
LIBOCTET_SHARED = liboctet.so

all: $(LIBOCTET_STATIC) $(LIBOCTET_SHARED) stresstest

stresstest: stresstest.o $(LIBOCTET_STATIC)
	$(CXX) $(CXXFLAGS) -o stresstest $(LDFLAGS) stresstest.o -L. -loctet

clean:
	rm -f stresstest *.o $(LIBOCTET_STATIC) $(LIBOCTET_SHARED)

$(LIBOCTET_STATIC): octet.o
	$(AR) cru $@ $^
	ranlib $@

$(LIBOCTET_SHARED): octet.o
	$(CXX) $(CXXFLAGS) $(SOFLAGS) $(LDFLAGS) -shared -o $(LIBOCTET_SHARED) $^


# Generated from clang++ -MM *.cpp -std=c++11 -stdlib=libc++

octet.o: octet.cpp octet.hpp octet-core.hpp octet-private.hpp
stresstest.o: stresstest.cpp octet.hpp octet-core.hpp octet-private.hpp

