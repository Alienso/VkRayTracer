#include <iostream>
#include <cstdlib>

int main(int argc, char** argv){

    std::cout << "argc: " << argc << '\n';
    for (int i=0; i<argc; i++){
        std::cout << "arg" << i << '=' << (argv)[i] << '\n';
    }

    system("");
    return 0;
}