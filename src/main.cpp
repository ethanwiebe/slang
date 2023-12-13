#include "slang.h"
#include "debugger.h"

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

void ReplLoop(CodeInterpreter* interp,bool debug){
	std::cout << "====slang v" SLANG_VERSION "====\n";
	std::string inputStr{};
	std::string secondStr{};
	SlangHeader* res;
	
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
			
			if (!interp->LoadExpr(inputStr)){
				interp->DisplayErrors();
				continue;
			}
			if (debug){
				uint8_t* start = interp->codeWriter.lambdaCodes[0].start;
				uint8_t* end = interp->codeWriter.lambdaCodes[0].write;
				PrintCode(start,end,std::cout);
			}
			
			if (!interp->Run()){
				interp->DisplayErrors();
				continue;
			}
			
			res = interp->PopArg();
			if (res){
				std::cout << *res << '\n';
				interp->SetGlobalSymbol("_",res);
			}
		}
	}
}

bool RunProgram(
		CodeInterpreter* interp,
		const std::string& filename,
		const std::string& code,
		bool shouldDebug,
		bool interactive,
		SlangHeader** res){
	//if (shouldDebug)
		//interp->codeWriter.optimize = false;
	if (!interp->LoadProgram(filename,code)){
		interp->DisplayErrors();
		return false;
	}
	if (shouldDebug){
		/*for (size_t i=0;i<interp->codeWriter.lambdaCodes.size();++i){
			CodeBlock& block = interp->codeWriter.lambdaCodes[i];
			uint8_t* start = block.start;
			uint8_t* end = block.write;
			std::cout << "BLOCK " << i << " ";
			if (block.isClosure)
				std::cout << "C";
			if (block.isVariadic)
				std::cout << "V";
			if (block.isPure)
				std::cout << "P";
			std::cout << '\n';
			std::cout << interp->parser.GetSymbolString(block.name);
			std::cout << '\n';
			const auto& params = interp->codeWriter.lambdaCodes[i].params;
			for (size_t pIndex=0;pIndex<params.size;++pIndex){
				std::cout << interp->parser.GetSymbolString(params.data[pIndex]) << ' ';
			}
			std::cout << '\n';
			PrintCode(start,end,std::cout);
			std::cout << "END\n\n";
		}*/
		DebuggerLoop(interp);
		return true;
	}
	if (!interp->Run()){
		interp->DisplayErrors();
		if (interactive)
			ReplLoop(interp,shouldDebug);
		return false;
	}
	
	*res = interp->PopArg();
	if (interactive){
		if (*res)
			interp->SetGlobalSymbol("_",*res);
		ReplLoop(interp,shouldDebug);
	}
	return true;
}

int main(int argc,char** argv){
	std::cout << std::setprecision(16);
	bool showInfo = false;
	bool cmdlineProg = false;
	bool interactive = false;
	bool shouldDebug = false;
	
	std::vector<std::string> argVec{};
	for (ssize_t i=1;i<argc;++i){
		argVec.emplace_back(argv[i]);
	}
	std::vector<std::string> filenames{};
	filenames.reserve(64);
	std::string prog{};
	prog.reserve(64);
	for (size_t i=0;i<argVec.size();++i){
		if (argVec[i]=="--info")
			showInfo = true;
		else if (argVec[i]=="-p"||argVec[i]=="--program"){
			if (!filenames.empty()){
				std::cout << "slang: -p must come before any programs!";
				return 1;
			}
			cmdlineProg = true;
		} else if (argVec[i]=="-i"||argVec[i]=="--interactive")
			interactive = true;
		else if (argVec[i]=="-g")
			shouldDebug = true;
		else if (argVec[i].starts_with("-")){
			std::cout << "slang: unknown command line arg " << argVec[i] << "\n";
			return 1;
		} else {
			if (cmdlineProg)
				prog += argVec[i];
			else
				filenames.push_back(argVec[i]);
		}
	}
	
	if (cmdlineProg&&prog.empty()){
		std::cout << "slang: expected program after -p\n";
		return 1;
	}
	
	if (filenames.empty()&&!cmdlineProg){
		interactive = true;
	}
	
	SlangHeader* res = nullptr;
	
	CodeInterpreter* interp = new CodeInterpreter();
	if (!cmdlineProg&&!filenames.empty()){
		for (const auto& filename : filenames){
			std::string code;
			{
				std::ifstream f(filename,std::ios::ate|std::ios::binary);
				if (!f){
					std::cout << "slang: cannot open file " << filename << "\n";
					return false;
				}
				auto size = f.tellg();
				f.seekg(0);
				code = std::string(size,'\0');
				f.read(code.data(),size);
			}
			if (!RunProgram(interp,filename,code,shouldDebug,interactive,&res))
				return 1;
		}
	} else if (cmdlineProg){
		if (!RunProgram(interp,"<cmdline>",prog,shouldDebug,interactive,&res))
			return 1;
		if (res){
			std::cout << *res << '\n';
		}
	} else {
		ReplLoop(interp,shouldDebug);
	}
	
	if (showInfo)
		PrintInfo();
		
	delete interp;
	
	return 0;
}
