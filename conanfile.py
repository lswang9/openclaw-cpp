from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class OpenclawCppConan(ConanFile):
    name = "openclaw-cpp"
    version = "1.113.0"
    package_type = "application"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    # Slack Socket Mode, Discord gateway/voice and Feishu WS use curl_ws_*; conan libcurl ships it off by default.
    default_options = {"libcurl/*:with_websockets": True}

    def requirements(self):
        self.requires("nlohmann_json/3.11.3")
        self.requires("libcurl/8.12.1")  # >=8.11: WebSocket API non-experimental (8.10 package has no ws)
        self.requires("opus/1.5.2")  # Discord voice encode/decode
        self.requires("zlib/[>=1.2.11 <2]")  # WA Web compressed frames (also a libcurl dep)
        self.requires("llama-cpp/b6565")  # local GGUF embedding inference (memorySearch provider "gguf")

    def configure(self):
        self.settings.compiler.cppstd = "20"

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
