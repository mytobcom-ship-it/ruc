CXX			= g++

CFLAGS		= -m64 -Wall -g3 -O0 -std=c++11 -Wno-write-strings -I. -I/usr/include -I/usr/pgsql-18/include
LFLAGS		= -lm -lpthread -L/usr/lib64/ -L/usr/pgsql-18/lib -lpq -Wl,-rpath,/usr/pgsql-18/lib

TARGET		= MapMatchSvr

SOURCES		= \
		Mutex.cpp \
		Condition.cpp \
		Clock.cpp \
		CodeMap.cpp \
		log4z.cpp \
		LoggerManager.cpp \
		CoordConvert.cpp \
		Coordinate.cpp \
		Util.cpp \
		GISUtil.cpp \
		IniReader.cpp \
		Thread.cpp \
		ThreadPool.cpp \
		SingleThread.cpp \
		SQLAccessor.cpp \
		PostgrePool.cpp \
		DataLoader.cpp \
		ContinueMapMatch.cpp \
		BeginMapMatch.cpp \
		MatchTrace.cpp \
		MapMatch.cpp \
		ProcessManager.cpp \
		RawLogFetcher.cpp \
		RawLogWorker.cpp \
		Server.cpp \
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
	rm -rf *.o *.lis
