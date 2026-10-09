CXX      ?= g++
CXXFLAGS += -std=c++17 -pthread
CPPFLAGS += -Isrc

SRC := $(shell find src -name '*.cpp')
OBJ := $(SRC:.cpp=.o)
DEPS := $(OBJ:.o=.d)

dog: $(OBJ)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(OBJ) -o $@ -lcurl -pthread

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f $(OBJ) $(DEPS) dog

.PHONY: clean

-include $(DEPS)
