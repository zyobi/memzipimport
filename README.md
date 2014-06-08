# Purpose

This module is mostly the same like the builtin module `zipimport`. While `zipimport` imports from a zip file, and `memzipimport` imports from zipped content in memory.

# Usage

    import sys
    sys.memzip = {"library.zip": bytearray(open("lib.zip", "rb").read())}
    sys.path.insert(0, "library.zip")
    sys.path_hooks.insert(0, memzipimport.zipimporter)
    
There are three steps:

* store zipped content in sys.memzip as an bytearray
* insert a virtual library path
* insert path hooks

