CXX			= g++

CFLAGS		= -m64 -Wall -g3 -O2 -std=c++11 -Wno-write-strings -I/usr/include -I/usr/pgsql-18/include
LFLAGS		= -lm -lpthread -L/usr/lib64/ -L/usr/pgsql-18/lib -lpq -Wl,-rpath,/usr/pgsql-18/lib

TARGET		= RawGpsSimSvr

SOURCES		= \
		log4z.cpp \
		Mutex.cpp \
		Condition.cpp \
		IniReader.cpp \
		SQLAccessor.cpp \
		PostgrePool.cpp \
		GeoUtil.cpp \
		RouteProvider.cpp \
		Vehicle.cpp \
		SimServer.cpp \
		AppMain.cpp

OBJECTS	= $(SOURCES:.cpp=.o)

.SUFFIXES: .cpp .c .o

all: $(TARGET)
	rm -rf *.o *.lis

$(TARGET):$(OBJECTS)
	$(CXX) -o $@ $^ $(LFLAGS)

install: all
	mv $(TARGET) ../bin/
	chmod +x ../bin/$(TARGET)
	rm -rf *.o *.lis
	@echo "**********Compile Success**********"

.cpp.o:
	$(CXX) -c $(CFLAGS) $<

clean:
	rm -rf *.o *.lis $(TARGET)
