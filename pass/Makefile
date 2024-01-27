CXX := /usr/bin/clang++
CXXFLAGS = $(shell llvm-config --cxxflags) -Wall -pedantic-errors -fPIC -std=c++20
CXXLIBFLAGS = -dylib -shared

LIB_DIR := lib
OBJ_DIR := obj

LIB_SRC := $(LIB_DIR)/LoopBlocking.cpp
LIB_INCLUDE := $(LIB_SRC:.cpp=.hpp)
LIB_OBJ := $(LIB_SRC:$(LIB_DIR)/%.cpp=$(OBJ_DIR)/%.o)

PASS := LoopBlocking.so

all: $(PASS)
all-debug: CXXFLAGS += -g
all-debug: CXXLIBFLAGS += -g
all-debug: all

$(PASS): $(LIB_OBJ)
	$(CXX) $(CXXLIBFLAGS) $^ -o $@

$(LIB_OBJ): $(LIB_SRC) $(LIB_INCLUDE) | $(OBJ_DIR) $(LIB_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $@

.PHONY: clean all all-debug

clean:
	$(RM) $(PASS) $(OBJ_DIR)/*

