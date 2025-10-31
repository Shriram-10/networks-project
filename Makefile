# Makefile for Fast File Transfer over UDP
# NIT-Trichy Computer Networks Project

# Compiler and flags
CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -pthread -std=c++11
LDFLAGS = -pthread

# Target executables
TARGETS = sender receiver garbler

# Source files
SENDER_SRC = sender.cpp
RECEIVER_SRC = receiver.cpp
GARBLER_SRC = garbler.cpp

# Header files
HEADERS = protocol.h

# Default target - build all executables
all: $(TARGETS)
	@echo ""
	@echo "=== Build Complete ==="
	@echo "Executables created:"
	@echo "  - sender    : File sender"
	@echo "  - receiver  : File receiver"
	@echo "  - garbler   : Packet loss simulator"
	@echo ""
	@echo "Run 'make test' for a quick test"
	@echo "Run 'make clean' to remove executables"

# Build sender
sender: $(SENDER_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o sender $(SENDER_SRC) $(LDFLAGS)
	@echo "Built: sender"

# Build receiver
receiver: $(RECEIVER_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o receiver $(RECEIVER_SRC) $(LDFLAGS)
	@echo "Built: receiver"

# Build garbler
garbler: $(GARBLER_SRC)
	$(CXX) $(CXXFLAGS) -o garbler $(GARBLER_SRC) $(LDFLAGS)
	@echo "Built: garbler"

# Create a test file
testfile:
	@dd if=/dev/urandom of=testfile.bin bs=1M count=10 2>/dev/null
	@echo "Created testfile.bin (10 MB)"

# Clean build artifacts
clean:
	rm -f $(TARGETS)
	rm -f testfile.bin
	rm -f garbler.log
	rm -f *.o
	@echo "Cleaned build artifacts"

# Clean test files
cleantest:
	rm -f testfile.bin received_*
	rm -f garbler.log
	@echo "Cleaned test files"

# Full clean
distclean: clean cleantest
	@echo "Full cleanup complete"

# Help target
help:
	@echo "Fast File Transfer over UDP - Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  make              - Build all executables"
	@echo "  make sender       - Build only sender"
	@echo "  make receiver     - Build only receiver"
	@echo "  make garbler      - Build only garbler"
	@echo "  make test         - Quick test without packet loss"
	@echo "  make test-garbler - Test with 10% packet loss"
	@echo "  make testfile     - Create a 10MB random test file"
	@echo "  make clean        - Remove executables"
	@echo "  make cleantest    - Remove test files"
	@echo "  make distclean    - Remove all generated files"
	@echo "  make help         - Show this help message"
	@echo ""
	@echo "Usage Examples:"
	@echo "  # Direct connection (no packet loss)"
	@echo "  Terminal 1: ./receiver 8080"
	@echo "  Terminal 2: ./sender 127.0.0.1 8080 myfile.bin 1024 5000"
	@echo ""
	@echo "  # With garbler (simulate network loss)"
	@echo "  Terminal 1: ./receiver 8080"
	@echo "  Terminal 2: ./garbler 9000 127.0.0.1 8080 0.15"
	@echo "  Terminal 3: ./sender 127.0.0.1 9000 myfile.bin 1024 5000"

# Phony targets
.PHONY: all clean cleantest distclean test test-garbler testfile help