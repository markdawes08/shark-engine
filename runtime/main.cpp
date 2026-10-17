#include <shark/bootstrap.hpp>

#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
    std::vector<std::string_view> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    switch (shark::parse_bootstrap_arguments(arguments)) {
    case shark::BootstrapAction::version:
        std::cout << "Shark Runtime " << shark::version_string() << '\n';
        return 0;
    case shark::BootstrapAction::help:
        std::cout
            << "Usage: Runtime [--help | --version]\n"
               "Shark build foundation only; game loading and rendering are not implemented.\n";
        return 0;
    case shark::BootstrapAction::invalid:
        std::cerr << "Unsupported arguments. Usage: Runtime [--help | --version]\n";
        return 2;
    }
    return 2;
}
