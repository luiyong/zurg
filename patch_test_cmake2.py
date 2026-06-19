import sys

with open('test/CMakeLists.txt', 'r') as f:
    content = f.read()

content = content.replace("message(FATAL_ERROR \"spdlog headers not found; install libspdlog-dev 或先配置顶层工程\")", "")

with open('test/CMakeLists.txt', 'w') as f:
    f.write(content)
