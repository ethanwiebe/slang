#include "slang.h"

#include <fstream>
#include <iostream>
#include <iomanip>

using namespace slang;

inline size_t BracketsAreComplete(const std::string& str){
	ssize_t score = 0;
	bool inStr = false;
	bool escapeNext = false;
	for (auto c : str){
		if (inStr){
			if (escapeNext){
				escapeNext = false;
				continue;
			}
			if (c=='"'){
				inStr = false;
			} else if (c=='\\'){
				escapeNext = true;
			}
			
			continue;
		}
		if (c=='"') inStr = true;
		else if (c=='('||c=='['||c=='{')
			++score;
		else if (c==')'||c==']'||c=='}')
			--score;
	}
	
	if (score<0) score = 0;
	return score;
}

void ReplLoop(SlangInterpreter* interp){
	std::cout << "====slang v0.0.0====\n";
	std::string inputStr{};
	std::string secondStr{};
	SlangHeader* res;
	SlangHeader* prog;
	
	while (true){
		res = nullptr;
		std::cout << "sl> ";
		inputStr.clear();
		std::getline(std::cin,inputStr);
		if (!inputStr.empty()){
			size_t b = 0;
			while ((b = BracketsAreComplete(inputStr))!=0){
				inputStr += '\n';
				for (size_t i=0;i<b;++i){
					std::cout << "\t";
				}
				std::getline(std::cin,secondStr);
				inputStr += secondStr;
			}
			
			prog = interp->Parse(inputStr);
			interp->Run(prog,&res);
			if (res){
				interp->SetGlobalSymbol("_",res);
				std::cout << *res << '\n';
			}
		}
	}
}

int main(int argc,char** argv){
	std::cout << std::setprecision(16);
	bool showInfo = false;
	bool cmdlineProg = false;
	bool interactive = false;
	
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
		else if (argVec[i]=="-i"||argVec[i]=="--interactive")
			interactive = true;
		else if (argVec[i].starts_with("-")){
			std::cout << "Unknown command line arg " << argVec[i] << "\n";
			return 1;
		}
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
		
		if (cmdlineProg&&interp.errors.empty())
			std::cout << *res << '\n';
			
		if (showInfo)
			PrintInfo();
			
		if (interactive){
			if (res)
				interp.SetGlobalSymbol("_",res);
			ReplLoop(&interp);
		}
		
		if (!interp.errors.empty())
			return 1;
			
	}
	
	return 0;
}
