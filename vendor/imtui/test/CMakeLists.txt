cmake_minimum_required(VERSION 3.10)
project(imtui_test_code_block VERSION 0.1.0 LANGUAGES CXX)

# Tests are run via Python (test_code_block.py) since imtui's stripped-down
# ImGui lacks the full API needed by imgui_markdown.
# The Python tests verify parsing logic by analyzing imgui_markdown.h source.
# Compilation is verified by the main llmfun_tui build.

# This CMakeLists exists so the test directory integrates with the build system.
# To run tests: python3 test_code_block.py
