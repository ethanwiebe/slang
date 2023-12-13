#include "debugger.h"

#include <iostream>
#include <sstream>
#include <charconv>

using namespace slang;
namespace slang {

void PrintStack(const Vector<SlangHeader*>& stack,size_t argsFrame,const Vector<StackData>& frames){
	size_t currFrameIndex = 0;
	for (size_t i=0;i<stack.size;++i){
		SlangHeader* v = stack.data[i];
		while (currFrameIndex<frames.size && i==frames.data[currFrameIndex].base){
			if (argsFrame==currFrameIndex)
				std::cout << "|";
			else
				std::cout << "/";
			++currFrameIndex;
		}
		if (!v){
			std::cout << "()";
		} else {
			std::cout << *v;
		}
		std::cout << "  ";
	}
	while (currFrameIndex<frames.size && stack.size==frames.data[currFrameIndex].base){
		if (argsFrame==currFrameIndex)
			std::cout << "|";
		else
			std::cout << "/";
		++currFrameIndex;
	}
	std::cout << '\n';
}

void PrintCodeSection(const std::string& str,size_t pcLine){
	const size_t width = 20;
	size_t currLine = 0;
	size_t center = pcLine;
	if (center<width/2) center = width/2;
	for (auto c : str){
		if (std::abs((ssize_t)(center-currLine))<=(ssize_t)width/2){
			std::cout << c;
		}
		if (c=='\n')
			++currLine;
	}
}

void PrintInterpState(CodeInterpreter* interp){
	std::stringstream output{};
	CodeBlock currCodeBlock = interp->codeWriter.lambdaCodes[interp->funcStack.Back().funcIndex];
	size_t pcLine = PrintCode(currCodeBlock.start,currCodeBlock.write,output,interp->pc);
	PrintCodeSection(output.str(),pcLine);
	size_t argsFrame = interp->funcStack.Back().argsFrame;
	PrintStack(interp->argStack,argsFrame,interp->stack);
	FuncData& fd = interp->funcStack.Back();
	if (interp->funcStack.size>1){
		size_t retFunc = interp->funcStack.data[interp->funcStack.size-2].funcIndex;
		std::cout << "ret: " << 
			(int64_t)(fd.retAddr-interp->codeWriter.lambdaCodes[retFunc].start) << 
			'\n';
	}
	std::cout << "pc: " << 
		(int64_t)(interp->pc-interp->codeWriter.lambdaCodes[fd.funcIndex].start) << '\n';
}

Vector<std::string_view> TokenizeCommand(const std::string& str){
	Vector<std::string_view> toks{};
	size_t startTok = -1ULL;
	for (size_t i=0;i<str.size();++i){
		char c = str[i];
		if ((c==' '||c=='\n'||c=='\t')&&startTok==-1ULL)
			continue;
		
		if ((c==' '||c=='\n'||c=='\t')){
			toks.PushBack({str.data()+startTok,str.data()+i});
			startTok = -1ULL;
		} else if (startTok==-1ULL)
			startTok = i;
	}
	if (startTok!=-1ULL){
		toks.PushBack({str.data()+startTok,str.data()+str.size()});
	}
	return toks;
}

void PrintBlock(const CodeInterpreter* interp,size_t i,const CodeBlock& block){
	const size_t funcNameJustSize = 20;
	std::cout << i << ' ';
	if (i<10)
		std::cout << ' ';
	if (i<100)
		std::cout << ' ';
	if (block.isPure)
		std::cout << 'P';
	else
		std::cout << ' ';
	if (block.isClosure)
		std::cout << 'C';
	else
		std::cout << ' ';
	if (block.isVariadic)
		std::cout << 'V';
	else
		std::cout << ' ';
	std::cout << "  ";
	std::string_view sv = interp->parser.GetSymbolString(block.name);
	std::cout << sv;
	ssize_t s = funcNameJustSize-sv.size();
	for (ssize_t j=0;j<s;++j)
		std::cout << ' ';
	std::cout << " (";
	const auto& params = block.params;
	for (size_t pIndex=0;pIndex<params.size;++pIndex){
		std::cout << interp->parser.GetSymbolString(params.data[pIndex]);
		if (pIndex!=params.size-1) std::cout << ' ';
	}
	std::cout << ")\n";
}

struct CodeLoc {
	size_t funcIndex;
	size_t offset;
};

CodeLoc ParseBreakpoint(std::string_view sv){
	size_t colonPos = sv.find(':');
	if (colonPos==std::string::npos)
		return {-1ULL,-1ULL};
	
	std::string_view first = {sv.data(),sv.data()+colonPos};
	std::string_view last = {sv.data()+colonPos+1,sv.data()+sv.size()};
	if (!first.size()||!last.size()){
		return {-1ULL,-1ULL};
	}
	size_t funcIndex = -1ULL;
	size_t offset = -1ULL;
	std::from_chars(first.data(),first.data()+first.size(),funcIndex);
	std::from_chars(last.data(),last.data()+last.size(),offset);
	return {funcIndex,offset};
}

size_t CheckBreakpoints(CodeInterpreter* interp,const Vector<CodeLoc>& breaks){
	size_t currFunc = interp->funcStack.Back().funcIndex;
	size_t currOffset = interp->pc-interp->codeWriter.lambdaCodes[currFunc].start;
	for (size_t i=0;i<breaks.size;++i){
		if (breaks.data[i].funcIndex==currFunc&&breaks.data[i].offset==currOffset)
			return i;
	}
	return -1ULL;
}

void DebuggerLoop(CodeInterpreter* interp){
	std::string inputStr{};
	const std::vector<CodeBlock>& codes = interp->codeWriter.lambdaCodes;
	std::cout << codes.size() << " blocks\n\n";
	bool running = false;
	Vector<CodeLoc> breaks{};
	PrintInterpState(interp);
	while (true){
		std::cout << "dbg> ";
		
		inputStr.clear();
		std::getline(std::cin,inputStr);
		if (inputStr.empty()){
			PrintInterpState(interp);
			continue;
		}
		
		if (inputStr=="q"){
			return;
		}
		
		if (inputStr=="s"){
			bool r = interp->Step();
			PrintInterpState(interp);
			if (!r){
				if (interp->errors.empty()){
					std::cout << "HALTED\n";
					SlangHeader* ret = interp->PopArg();
					if (ret)
						std::cout << *ret << '\n';
				} else {
					interp->DisplayErrors();
				}
				return;
			}
		} else if (inputStr=="r"){
			running = true;
		} else if (inputStr=="b"){
			if (breaks.size==0){
				std::cout << "No active breakpoints\n";
				continue;
			}
			std::cout << "Active breakpoints:\n";
			for (size_t i=0;i<breaks.size;++i){
				std::cout << i << ": ";
				if (i<10) std::cout << " ";
				std::cout << breaks.data[i].funcIndex << ":" << breaks.data[i].offset;
				std::cout << '\n';
			}
		} else if (inputStr.starts_with("b ")){
			auto tokens = TokenizeCommand(inputStr);
			if (tokens.size==3){
				if (tokens.data[1]!="del"){
					std::cout << "Breakpoint syntax: b block:offset | b del block:offset\n";
					continue;
				}
				std::string_view bpNum = tokens.data[2];
				size_t num = -1ULL;
				std::from_chars(bpNum.data(),bpNum.data()+bpNum.size(),num);
				if (num>=breaks.size){
					std::cout << "Invalid breakpoint number: " << (ssize_t)num << '\n';
					continue;
				}
				breaks.PopFrom(num);
				std::cout << "Deleted breakpoint\n";
			} else if (tokens.size==2){
				CodeLoc b = ParseBreakpoint(tokens.data[1]);
				if (b.funcIndex==-1ULL||b.offset==-1ULL){
					std::cout << "Breakpoint syntax: b block:offset | b del block:offset\n";
					continue;
				}
				breaks.PushBack(b);
				std::cout << "Added breakpoint\n";
			} else {
				std::cout << "Breakpoint syntax: b block:offset | b del block:offset\n";
			}
		} else if (inputStr=="blocks"){
			std::cout << "Blocks:\n";
			for (size_t i=0;i<codes.size();++i){
				auto& block = codes[i];
				PrintBlock(interp,i,block);
			}
		} else if (inputStr=="block"){
			std::cout << "Block syntax: block index\n";
		} else if (inputStr.starts_with("block ")){
			auto tokens = TokenizeCommand(inputStr);
			if (tokens.size!=2) continue;
			ssize_t num = -1;
			std::from_chars(tokens.data[1].data(),tokens.data[1].data()+tokens.data[1].size(),num);
			if (num<0||num>=(ssize_t)codes.size()){
				std::cout << "Invalid block: " << num << '\n';
				continue;
			}
			PrintBlock(interp,num,codes[num]);
			PrintCode(codes[num].start,codes[num].write,std::cout,interp->pc);
		} else {
			std::cout << "Unrecognized command: " << inputStr << '\n';
		}
		
		while (running){
			bool r = interp->Step();
			if (!r){
				if (interp->errors.empty()){
					std::cout << "HALTED\n";
					SlangHeader* ret = interp->PopArg();
					if (ret)
						std::cout << *ret << '\n';
				} else {
					interp->DisplayErrors();
				}
				return;
			}
			size_t breakPoint = CheckBreakpoints(interp,breaks);
			if (breakPoint!=-1ULL){
				running = false;
				PrintInterpState(interp);
				std::cout << "Hit breakpoint " << breakPoint << '\n';
			}
		}
	}
	
}

}
