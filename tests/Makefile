# ============================================================================
#  Makefile for EV Controller desktop unit tests
#
#  Usage:
#    make          - Build all tests
#    make run      - Build and run all tests
#    make clean    - Remove built binaries
#    make verbose  - Run tests with -Wall -Wextra warnings shown
#
#  Requirements: g++ (any modern version with C++11 support)
# ============================================================================

CXX      := g++
CXXFLAGS := -std=c++11 -Wall -Wextra -O2
FW       := ../firmware

# All test binaries
TESTS := test_util_math test_cfg_settings test_svc_ramp test_util_ring test_svc_speed test_svc_pedals test_cfg_params test_app_protocol

.PHONY: all run clean verbose help

all: $(TESTS)

# --- Individual tests ---

test_util_math: test_util_math.c $(FW)/util_math.cpp
	$(CXX) $(CXXFLAGS) -I$(FW) -o $@ $^

test_cfg_settings: test_cfg_settings.c $(FW)/cfg_settings.cpp $(FW)/cfg_params.cpp $(FW)/util_crc.cpp $(FW)/util_rom.cpp
	$(CXX) $(CXXFLAGS) -I$(FW) -o $@ $^

test_svc_ramp: test_svc_ramp.cpp $(FW)/svc_ramp.cpp $(FW)/util_math.cpp
	$(CXX) $(CXXFLAGS) -I$(FW) -o $@ $^

test_util_ring: test_util_ring.c $(FW)/util_ring.cpp
	$(CXX) $(CXXFLAGS) -I$(FW) -o $@ $^

test_svc_speed: test_svc_speed.c $(FW)/svc_speed.cpp
	$(CXX) $(CXXFLAGS) -I$(FW) -o $@ $^

test_svc_pedals: test_svc_pedals.c $(FW)/svc_pedals.cpp $(FW)/cfg_settings.cpp $(FW)/cfg_params.cpp $(FW)/util_crc.cpp $(FW)/util_math.cpp $(FW)/util_rom.cpp
	$(CXX) $(CXXFLAGS) -I$(FW) -o $@ $^

test_cfg_params: test_cfg_params.c $(FW)/cfg_settings.cpp $(FW)/cfg_params.cpp $(FW)/util_crc.cpp $(FW)/util_rom.cpp
	$(CXX) $(CXXFLAGS) -I$(FW) -o $@ $^

test_app_protocol: test_app_protocol.c $(FW)/app_protocol.cpp $(FW)/cfg_settings.cpp $(FW)/cfg_params.cpp $(FW)/svc_pedals.cpp $(FW)/svc_speed.cpp $(FW)/util_crc.cpp $(FW)/util_math.cpp $(FW)/util_rom.cpp
	$(CXX) $(CXXFLAGS) -I$(FW) -o $@ $^

# --- Run all ---

run: all
	@echo ""
	@echo "============================================"
	@echo "  Running test_util_math"
	@echo "============================================"
	@./test_util_math
	@echo ""
	@echo "============================================"
	@echo "  Running test_cfg_settings"
	@echo "============================================"
	@./test_cfg_settings
	@echo ""
	@echo "============================================"
	@echo "  Running test_svc_ramp"
	@echo "============================================"
	@./test_svc_ramp
	@echo ""
	@echo "============================================"
	@echo "  Running test_util_ring"
	@echo "============================================"
	@./test_util_ring
	@echo ""
	@echo "============================================"
	@echo "  Running test_svc_speed"
	@echo "============================================"
	@./test_svc_speed
	@echo ""
	@echo "============================================"
	@echo "  Running test_svc_pedals"
	@echo "============================================"
	@./test_svc_pedals
	@echo ""
	@echo "============================================"
	@echo "  Running test_cfg_params"
	@echo "============================================"
	@./test_cfg_params
	@echo ""
	@echo "============================================"
	@echo "  Running test_app_protocol"
	@echo "============================================"
	@./test_app_protocol
	@echo ""
	@echo "============================================"
	@echo "  All tests completed."
	@echo "============================================"

# --- Verbose build (show all warnings) ---

verbose: CXXFLAGS += -Wpedantic -Wshadow -Wcast-align -Wconversion
verbose: clean all

# --- Cleanup ---

clean:
	rm -f $(TESTS)

# --- Help ---

help:
	@echo "EV Controller Desktop Unit Tests"
	@echo ""
	@echo "Targets:"
	@echo "  make          Build all tests"
	@echo "  make run      Build and run all tests"
	@echo "  make verbose  Build with extra warnings"
	@echo "  make clean    Remove built binaries"
	@echo "  make help     Show this help"
