# PlatformIO extra script for env:pc: the OS libraries the PC platform needs.
Import("env")
import sys

env.Append(LIBS=["ws2_32"] if sys.platform == "win32" else ["pthread"])
