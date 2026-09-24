# PlatformIO extra script for env:pc: BearSSL (the ESP8266 core's fork,
# external/bearssl) built from source for lib/tinc_tls, so https behaves as it
# does on the board; then the OS libraries (after it, for the linker).
Import("env")
import os
import sys

root = env.subst("$PROJECT_DIR")
src = os.path.join(root, "external", "bearssl", "src")
bearssl = env.Clone()
bearssl.Append(CPPPATH=[src])  # its private inner.h
bearssl.Replace(CCFLAGS=[f for f in bearssl["CCFLAGS"] if f not in ("-Wall", "-Wextra")])
libs = []
if sys.platform == "win32":
    # MinGW has no <alloca.h>; and the fork's pgmspace.h defines PSTR, which
    # breaks <windows.h> unless windows.h is seen first
    win = ["rand/sysrng.c", "ssl/ssl_engine.c", "x509/x509_minimal.c"]
    bearssl.Append(CPPPATH=[os.path.join(root, "platforms", "pc", "compat")])
    w = bearssl.Clone()
    w.Append(CCFLAGS=["-include", "windows.h"])
    libs.append(bearssl.BuildLibrary(os.path.join("$BUILD_DIR", "bearssl"), src,
                                     src_filter="+<*> " + " ".join("-<%s>" % f for f in win)))
    libs.append(w.StaticLibrary(os.path.join("$BUILD_DIR", "bearssl_win"), [
        w.Object(os.path.join("$BUILD_DIR", "bearssl_win", f.replace("/", "_") + ".o"),
                 os.path.join(src, f)) for f in win]))
    libs += ["ws2_32", "advapi32"]
else:
    libs.append(bearssl.BuildLibrary(os.path.join("$BUILD_DIR", "bearssl"), src))
    libs.append("pthread")
env.Append(LIBS=libs)
