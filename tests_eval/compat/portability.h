#pragma once
/// Force-included before every test source by run_tests.sh (-include): headers
/// MinGW pulls in transitively but Linux libstdc++ does not, plus the
/// Win32 -> POSIX surface from windows.h.

#include <mutex>
#include <string>
#include <cstdint>

#include "windows.h"
