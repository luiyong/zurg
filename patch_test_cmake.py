import sys

with open('test/CMakeLists.txt', 'r') as f:
    content = f.read()

content = content.replace("find_package(Protobuf CONFIG REQUIRED)", "find_package(Protobuf REQUIRED)")

with open('test/CMakeLists.txt', 'w') as f:
    f.write(content)
