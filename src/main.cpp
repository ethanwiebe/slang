#include "slang.h"

#include <fstream>
#include <iostream>
#include <iomanip>

using namespace slang;

void ReplLoop(SlangInterpreter* interp){
	std::cout << "---REPL---\n";
	std::string inputStr{};
	SlangHeader* res;
	SlangHeader* prog;
	
	std::string code;
	while (true){
		std::cout << "sl> ";
		std::getline(std::cin,inputStr);
		prog = interp->Parse(inputStr);
		interp->Run(prog,&res);
		if (res)
			std::cout << *res << '\n';
	}
}

int main(int argc,char** argv){
	std::cout << std::setprecision(19);
	bool showInfo = false;
	bool cmdlineProg = false;
	
	std::vector<std::string> argVec{};
	for (ssize_t i=1;i<argc;++i){
		argVec.emplace_back(argv[i]);
	}
	std::vector<std::string> fileNames{};
	
	for (size_t i=0;i<argVec.size();++i){
		if (argVec[i]=="--info")
			showInfo = true;
		else if (argVec[i]=="-p"||argVec[i]=="--prog")
			cmdlineProg = true;
		else
			fileNames.push_back(argVec[i]);
	}
	
	if (fileNames.empty()){
		SlangInterpreter s{};
		ReplLoop(&s);
		
		return 0;
	}
	
	for (const auto& fileName : fileNames){
		std::string code;
		if (!cmdlineProg){
			std::ifstream f(fileName,std::ios::ate);
			if (!f){
				std::cout << "Cannot open file " << fileName << "!\n";
				return 1;
			}
			auto size = f.tellg();
			f.seekg(0);
			code = std::string(size,'\0');
			f.read(&code[0],size);
		} else {
			code = fileName;
		}
		SlangInterpreter interp = {};
		SlangHeader* res;
		SlangHeader* program = interp.Parse(code);
		if (program)
			interp.Run(program,&res);
		else
			return 1;
		
		if (showInfo)
			PrintInfo();
		if (!interp.errors.empty())
			return 1;
			
		if (cmdlineProg)
			std::cout << *res << '\n';
	}
	
	return 0;
}
