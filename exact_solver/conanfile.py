from conan import ConanFile
from conan.tools.cmake import cmake_layout

class CutwidthExactRecipe(ConanFile):
    name = "cutwidth_exact"
    version = "0.1.0"
    settings = "os", "compiler", "build_type", "arch"
    options = {"with_sdp": [True, False]}
    default_options = {"with_sdp": False}
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        self.requires("dispenso/1.5.1")
        self.requires("boost/1.86.0")
        if self.options.with_sdp:
            self.requires("eigen/3.4.0")
            self.requires("openblas/0.3.30")

    def layout(self):
        cmake_layout(self)
