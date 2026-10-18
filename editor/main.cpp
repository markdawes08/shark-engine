#include <shark/bootstrap.hpp>
#include <shark/client/bootstrap.hpp>
#include <shark/tools/build_info.hpp>

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
        std::cout << shark::tools::editor_version_banner() << '\n';
        return 0;
    case shark::BootstrapAction::help:
        std::cout << shark::client::editor_help();
        return 0;
    case shark::BootstrapAction::invalid:
        std::cerr << "Unsupported arguments. Usage: Editor [--help | --version]\n";
        return 2;
    }
    return 2;
}
