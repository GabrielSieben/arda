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

test/test_shell: test/test_shell.cpp Arda.cpp Arda.h test/Arduino.h
	$(CXX) $(CXXFLAGS) -o $@ test/test_shell.cpp

test/test_shell_minimal: test/test_shell_minimal.cpp Arda.cpp Arda.h test/Arduino.h
	$(CXX) $(CXXFLAGS) -o $@ test/test_shell_minimal.cpp

test/test_no_shell: test/test_no_shell.cpp Arda.cpp Arda.h test/Arduino.h
	$(CXX) $(CXXFLAGS) -o $@ test/test_no_shell.cpp

test/test_short_errors: test/test_short_errors.cpp Arda.cpp Arda.h test/Arduino.h
	$(CXX) $(CXXFLAGS) -o $@ test/test_short_errors.cpp

test/test_yield: test/test_yield.cpp Arda.cpp Arda.h test/Arduino.h
	$(CXX) $(CXXFLAGS) -o $@ test/test_yield.cpp

test/test_shell_manual_start: test/test_shell_manual_start.cpp Arda.cpp Arda.h test/Arduino.h
	$(CXX) $(CXXFLAGS) -o $@ test/test_shell_manual_start.cpp

# Build all test binaries
build: test/test_arda test/test_example_compile test/test_case_insensitive test/test_custom_name_len test/test_no_global_instance test/test_no_priority test/test_no_names test/test_priority_example test/test_shell test/test_shell_minimal test/test_no_shell test/test_short_errors test/test_yield test/test_shell_manual_start

# Run main tests
test: test/test_arda
	./test/test_arda

# Run yield tests (opt-in feature)
test-yield: test/test_yield
	./test/test_yield

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
	./test/test_shell
	./test/test_shell_minimal
	./test/test_no_shell
	./test/test_short_errors
	./test/test_yield
	./test/test_shell_manual_start

# Clean build artifacts
clean:
	rm -f test/test_arda test/test_example_compile test/test_case_insensitive \
	      test/test_custom_name_len test/test_no_global_instance test/test_no_global \
	      test/test_no_priority test/test_no_names test/test_priority_example \
	      test/test_shell test/test_shell_minimal test/test_no_shell test/test_short_errors \
	      test/test_yield test/test_shell_manual_start test/test_arda_cov test/*_bin \
	      test/*.gcov test/*.gcda test/*.gcno *.o

.PHONY: all test test-all test-yield build clean
