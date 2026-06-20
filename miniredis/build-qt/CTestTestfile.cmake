# CMake generated Testfile for 
# Source directory: /home/wcl/miniredis
# Build directory: /home/wcl/miniredis/build-qt
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[miniredis_unit_tests]=] "/home/wcl/miniredis/build-qt/miniredis_unit_tests")
set_tests_properties([=[miniredis_unit_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/wcl/miniredis/CMakeLists.txt;77;add_test;/home/wcl/miniredis/CMakeLists.txt;0;")
subdirs("tools/qt_console")
