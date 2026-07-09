CXX			= g++

# 헤더 전용 공유 파일(TypeDefine.h, DataDefine.h, DataFormat.h) 참조용
SRCDIR		= ../src

# Oracle 미사용 : OCCI 헤더/라이브러리 불필요
CFLAGS		= -m64 -Wall -g3 -O0 -std=c++11 -Wno-write-strings -I. -I$(SRCDIR) -I/usr/include
LFLAGS		= -lm -lpthread

TARGET		= MakeBinary
CHECK_TARGET	= CheckPsf

# 모든 소스를 로컬(maker)에서 컴파일
# (소스+헤더가 함께 있는 공유 모듈은 maker 로 복사하여 동일명으로 사용)
SOURCES		= \
		log4z.cpp \
		Clock.cpp \
		CoordConvert.cpp \
		IniReader.cpp \
		GISUtil.cpp \
		ShapeFile.cpp \
		ShapeLoader.cpp \
		TurnInfoLoader.cpp \
		BinaryMaker.cpp \
		MakeBinary.cpp

OBJECTS		= $(SOURCES:.cpp=.o)
CHECK_OBJECTS	= CheckPsf.o GISUtil.o

.SUFFIXES: .cpp .o

all: $(TARGET)
	rm -rf *.o *.lis

checkpsf: $(CHECK_TARGET)
	rm -rf CheckPsf.o GISUtil.o *.lis
	@echo "**********CheckPsf Compile Success**********"

$(TARGET): $(OBJECTS)
	$(CXX) -o $@ $^ $(LFLAGS)
	@echo "**********Compile Success**********"

$(CHECK_TARGET): $(CHECK_OBJECTS)
	$(CXX) -o $@ $^ $(LFLAGS)

install: all
	mkdir -p ../bin
	mv $(TARGET) ../bin/
	chmod +x ../bin/$(TARGET)
	@echo "**********Install Success**********"

.cpp.o:
	$(CXX) -c $(CFLAGS) $<

install-checkpsf: checkpsf
	mkdir -p ../bin
	mv $(CHECK_TARGET) ../bin/
	chmod +x ../bin/$(CHECK_TARGET)
	@echo "**********CheckPsf Install Success**********"

clean:
	rm -rf *.o *.lis $(TARGET) $(CHECK_TARGET)
