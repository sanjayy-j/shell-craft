# Makefile for ShellCraft

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g

TARGET   := shellcraft
SRCDIR   := src
OBJDIR   := build
TESTDIR  := tests
TEST_PARSER_BIN := $(OBJDIR)/test_parser

SRCS := $(wildcard $(SRCDIR)/*.cpp)
OBJS := $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(SRCS))

.PHONY: all clean run test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(TEST_PARSER_BIN): $(TESTDIR)/test_parser.cpp $(OBJDIR)/parser.o | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -I$(SRCDIR) -o $@ $^

run: $(TARGET)
	./$(TARGET)

test: $(TARGET) $(TEST_PARSER_BIN)
	bash $(TESTDIR)/run_tests.sh

clean:
	rm -rf $(OBJDIR) $(TARGET)
