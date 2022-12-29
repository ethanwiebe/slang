#include "slang.h"

#include <fstream>
#include <iostream>
#include <iomanip>

using namespace slang;

int main(int argc,char** argv){
	if (argc<=1)
		return 0;
		
	std::cout << std::setprecision(19);
	
	std::string fileName = std::string(argv[1]);
	
	std::ifstream f(fileName,std::ios::ate);
	auto size = f.tellg();
	f.seekg(0);
	std::string code(size,'\0');
	f.read(&code[0],size);
	
	SlangInterpreter interp = {};
	SlangObj* program = interp.Parse(code);
	if (program)
		interp.Run(program);
	else
		return 1;
	
	return 0;
}
