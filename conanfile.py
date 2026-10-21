from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class OpenclawCppConan(ConanFile):
    name = "openclaw-cpp"
    version = "1.56.0"
    package_type = "application"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        self.requires("nlohmann_json/3.11.3")
        self.requires("libcurl/8.10.1")

    def configure(self):
        self.settings.compiler.cppstd = "20"

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
