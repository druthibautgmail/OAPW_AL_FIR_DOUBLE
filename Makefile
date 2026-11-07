CXX = g++
# Optimierte Flags fuer DSP auf ARM64
# -mcpu=native: Code passt nur zur CPU, auf der kompiliert wird -> auf jedem Pi selbst bauen
# -fno-finite-math-only: Pflicht zu -ffast-math, sonst entfernt der Compiler NaN/Inf-Pruefungen
# -MMD -MP: erzeugt .d-Dateien, damit Aenderungen an Headern (z.B. DeviceConfig.h) automatisch neu bauen
CXXFLAGS = -Wall -O3 -std=c++17 -Iinclude -mcpu=native -ffast-math -fno-finite-math-only -flto -fopenmp -MMD -MP
LDFLAGS = -lasound -lpthread -flto -fopenmp -lfftw3 -lm -lsndfile

TARGET = OAPW_Player
SRCS = src/main.cpp src/AlsaCapture.cpp src/AlsaPlayback.cpp src/RACEDspEngine.cpp src/FirEq.cpp
OBJS = $(SRCS:.cpp=.o)
DEPS = $(OBJS:.o=.d)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

-include $(DEPS)
