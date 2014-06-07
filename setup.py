from distutils.core import setup, Extension

memzipimport = Extension("memzipimport",
                       sources=["memzipimport.c"],
                       extra_compile_args=["/W4", # Highest warning level
                                           "/Zi",  # Generate PDBs
                                          ],
                      )

setup(name="memzipimport",
      version="0.1.0",
      description="import zip from memory",
      maintainer="Robin",
      maintainer_email="zyobi@163.com",
      ext_modules=[memzipimport],
     )

