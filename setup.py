import sys
from setuptools import setup, Extension

extra_compile_args = []
if sys.platform != 'win32':
    extra_compile_args.extend(["-Wall", "-g"])
else:
    extra_compile_args.extend(["/W4", "/Zi"])

memzipimport = Extension("memzipimport",
                       sources=["memzipimport.c"],
                       extra_compile_args=extra_compile_args,
                      )

setup(name="memzipimport",
      version="0.1.0",
      description="import zip from memory",
      maintainer="Robin",
      maintainer_email="zyobi@163.com",
      ext_modules=[memzipimport],
     )
