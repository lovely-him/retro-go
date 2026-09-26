# This file is injected late into rg_tool.py, you can run arbitrary python code here
# For example override python variables or set environment variables with os.putenv

# Espressif chip in the device
IDF_TARGET = "esp32p4"
# .fw file format, if supported by the device
FW_FORMAT = "none"
# Default apps to build when none is specified (comment to build all)
# DEFAULT_APPS = "launcher retro-core"

# 覆盖默认分区表大小, 避免编译警告
PROJECT_APPS = {
  'launcher':   [0, 16, 0x140000],  #  0x* = * KB -> * KB    *%
  'retro-core': [0, 17, 0x140000],  #  0x* = * KB -> * KB    *%
  'prboom-go':  [0, 18, 0x140000],  #  0x* = * KB -> * KB    *%
  'gwenesis':   [0, 19, 0x140000],  #  0x* = * KB -> * KB    *%
  'fmsx':       [0, 20, 0x140000],  #  0x* = * KB -> * KB    *%
}

