#include <string>
#include <iostream>
#include <fstream>
#include <streambuf>
#include <algorithm>

#include <evm2wasm.h>

using namespace std;


int main(int argc, char **argv) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <EVM file> [--tracing]" << endl;
        return 1;
    }

    bool tracing = false;
    if (argc == 3) {
        tracing = (string(argv[2]) == "--tracing");
    }
    ifstream input(argv[1]);
    if (!input.is_open()) {
        cerr << "File not found: " << argv[1] << endl;
        return 1;
    }

    string str(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>()
    );
    str.erase(remove_if(str.begin(), str.end(), [](unsigned char x){return std::isspace(x);}), str.end());
    cout << evm2wasm::evmhex2wast(str, tracing);
    return 0;
}
