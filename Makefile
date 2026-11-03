CXX = g++
# Optimierte Flags für DSP auf ARM64
CXXFLAGS = -Wall -O3 -std=c++17 -Iinclude -mcpu=native -ffast-math -flto -fopenmp
# NEU: FFTW3 (Float) und Math-Lib hinzugefügt
LDFLAGS = -lasound -lpthread -flto -fopenmp -lfftw3 -lm

TARGET = OAPW_Player
SRCS = src/main.cpp src/AlsaCapture.cpp src/AlsaPlayback.cpp src/RACEDspEngine.cpp src/FirEq.cpp
OBJS = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
