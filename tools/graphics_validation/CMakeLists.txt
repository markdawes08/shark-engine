cmake_minimum_required(VERSION 3.28)
project(SharkGraphicsValidation LANGUAGES C CXX)

# This project is deliberately never added to Shark's engine target graph.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/GraphicsValidation.cmake")
shark_setup_graphics_validation()
