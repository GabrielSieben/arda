CXX = g++
CXXFLAGS = -std=c++11 -Wall -Wextra -I. -Itest

# Default target
all: test

# Main test binary (unity build - test file includes Arda.cpp directly)
test/test_arda: test/test_arda.cpp Arda.cpp Arda.h test/Arduino.h
	$(CXX) $(CXXFLAGS) -o $@ test/test_arda.cpp

# Example compilation test
test/test_example_compile: test/test_example_compile.cpp Arda.cpp Arda.h test/Arduino.h
	$(CXX) $(CXXFLAGS) -o $@ test/test_example_compile.cpp

# Configuration variant tests
test/test_case_insensitive: test/test_case_insensitive.cpp Arda.cpp Arda.h test/Arduino.h
	$(CXX) $(CXXFLAGS) -o $@ test/test_case_insensitive.cpp

test/test_custom_name_len: test/test_custom_name_len.cpp Arda.cpp Arda.h test/Arduino.h
	$(CXX) $(CXXFLAGS) -o $@ test/test_custom_name_len.cpp

test/test_no_global_instance: test/test_no_global_instance.cpp Arda.cpp Arda.h test/Arduino.h
	$(CXX) $(CXXFLAGS) -o $@ test/test_no_global_instance.cpp

test/test_no_priority: test/test_no_priority.cpp Arda.cpp Arda.h test/Arduino.h
	$(CXX) $(CXXFLAGS) -o $@ test/test_no_priority.cpp

test/test_no_names: test/test_no_names.cpp Arda.cpp Arda.h test/Arduino.h
	$(CXX) $(CXXFLAGS) -o $@ test/test_no_names.cpp

test/test_priority_example: test/test_priority_example.cpp Arda.cpp Arda.h test/Arduino.h
	$(CXX) $(CXXFLAGS) -o $@ test/test_priority_example.cpp

# Build all test binaries
build: test/test_arda test/test_example_compile test/test_case_insensitive test/test_custom_name_len test/test_no_global_instance test/test_no_priority test/test_no_names test/test_priority_example

# Run main tests
test: test/test_arda
	./test/test_arda

# Run all tests including configuration variants
test-all: build
	./test/test_arda
	./test/test_example_compile
	./test/test_case_insensitive
	./test/test_custom_name_len
	./test/test_no_global_instance
	./test/test_no_priority
	./test/test_no_names
	./test/test_priority_example

# Clean build artifacts
clean:
	rm -f test/test_arda test/test_example_compile test/test_case_insensitive \
	      test/test_custom_name_len test/test_no_global_instance test/test_no_priority \
	      test/test_no_names test/test_priority_example test/test_arda_cov \
	      test/*.gcov test/*.gcda test/*.gcno *.o

.PHONY: all test test-all build clean
