#include "slang.h"

#include <assert.h>
#include <iostream>
#include <cstring>
#include <sstream>
#include <math.h>
#include <chrono>

#include <set>

#define GEN_FLAG (1ULL<<63)
#define LET_SELF_SYM (GEN_FLAG)

namespace slang {

size_t gAllocCount = 0;
size_t gAllocTotal = 0;
size_t gTenureCount = 0;
size_t gHeapAllocTotal = 0;
size_t gMaxStackHeight = 0;
size_t gSmallGCs = 0;
size_t gReallocCount = 0;
size_t gMaxArenaSize = 0;
size_t gCurrDepth = 0;
size_t gMaxDepth = 0;
size_t gEnvCount = 0;
size_t gEnvBalance = 0;
double gParseTime = 0.0;
double gRunTime = 0.0;

size_t gEvalCounter = 0;
size_t gEvalRecurCounter = 0;

SlangParser* gDebugParser;
SlangInterpreter* gDebugInterpreter;

void PrintErrors(const std::vector<ErrorData>& errors){
	for (auto it=errors.rbegin();it!=errors.rend();++it){
		auto error = *it;
		std::cout << "@" << error.loc.line+1 << ',' << error.loc.col+1 <<
			":\n" << error.message << '\n';
	}
}

void PrintCurrEnv(const SlangEnv* env){
	if (env->header.type!=SlangType::Env){
		std::cout << "not env\n";
	}
	std::cout << env->header.varCount << ": ";
	for (size_t i=0;i<SLANG_ENV_BLOCK_SIZE;++i){
		if (i>=env->header.varCount){
			std::cout << "( [] : [] )";
			continue;
		}
		const auto& mapping = env->mappings[i];
		const std::string& name = gDebugParser->GetSymbolString(mapping.sym);
		if (!mapping.obj){
			std::cout << "( " << name << " : () ) ";
		} else {
			std::cout << "( " << name << " : " << *mapping.obj << " ) ";
		}
	}
	std::cout << '\n';
	if (env->next){
		PrintCurrEnv(env->next);
	}
	
	if (env->parent){
		std::cout << "\n\n";
		PrintCurrEnv(env->parent);
	}
}

inline const char* TypeToString(SlangType type){
	switch (type){
		case SlangType::Int:
			return "Int";
		case SlangType::Real:
			return "Real";
		case SlangType::Vector:
			return "Vec";
		case SlangType::String:
			return "Str";
		case SlangType::List:
			return "Pair";
		case SlangType::Bool:
			return "Bool";
		case SlangType::Symbol:
			return "Sym";
		case SlangType::Lambda:
			return "Proc";
		case SlangType::Env:
			return "Env";
		case SlangType::Params:
			return "Params";
		case SlangType::Storage:
			return "Storage";
		case SlangType::NullType:
			return "NullType";
	}
	
	static char errArr[32];
	sprintf(errArr,"Invalid %d",(int)type);
	return errArr;
}

inline size_t SlangHeader::GetSize() const {
	switch (type){
		case SlangType::Storage: {
			uint8_t* self = (uint8_t*)this;
			size_t cap = *(self+offsetof(SlangStorage,capacity));
			return sizeof(SlangStorage)+cap*elemSize;
		}
		case SlangType::Vector: {
			return sizeof(SlangVec);
		}
		case SlangType::String: {
			return sizeof(SlangStr);
		}
		case SlangType::Env: {
			return sizeof(SlangEnv);
		}
		case SlangType::Params: {
			uint8_t* self = (uint8_t*)this;
			size_t size = *(self+offsetof(SlangParams,size));
			return sizeof(SlangParams)+size*sizeof(SymbolName);
		}
		case SlangType::Int:
		case SlangType::Real:
		case SlangType::Symbol:
		case SlangType::Bool:
			return sizeof(SlangObj);
		case SlangType::Lambda:
			return sizeof(SlangLambda);
		case SlangType::List:
			return sizeof(SlangList);
		case SlangType::NullType:
			assert(false);
			return 0;
	}
	assert(false);
	return 0;
}

inline void* SlangInterpreter::Allocate(size_t mem){
	mem = QuantizeSize(mem);
	//SmallGC(mem);
	if (arena->currPointer+mem<arena->currSet+arena->memSize/2){
		gAllocTotal += mem;
		uint8_t* data = arena->currPointer;
		arena->currPointer += mem;
		return data;
	}
	
	SmallGC(mem);
	if (arena->currPointer+mem>=arena->currSet+arena->memSize/2){
		std::cout << "out of mem\n";
		exit(1);
		return nullptr;
	}
	
	gAllocTotal += mem;
	void* data = arena->currPointer;
	arena->currPointer += mem;
	return data;
}



inline SlangObj* SlangInterpreter::AllocateSmallObj(){
	SlangObj* p = (SlangObj*)Allocate(sizeof(SlangHeader)+sizeof(uint64_t));
	p->header.type = (SlangType)65;
	p->header.flags = 0;
	return p;
}

inline SlangLambda* SlangInterpreter::AllocateLambda(){
	SlangLambda* lam = (SlangLambda*)Allocate(sizeof(SlangLambda));
	lam->header.type = SlangType::Lambda;
	lam->header.flags = 0;
	return lam;
}

inline SlangList* SlangInterpreter::AllocateList(){
	SlangList* p = (SlangList*)Allocate(sizeof(SlangList));
	p->header.type = SlangType::List;
	p->header.flags = 0;
	return p;
}

inline SlangParams* SlangInterpreter::AllocateParams(size_t size){
	SlangParams* obj = (SlangParams*)Allocate(sizeof(SlangParams)+sizeof(SymbolName)*size);
	obj->header.type = SlangType::Params;
	obj->header.flags = 0;
	obj->size = size;
	return obj;
}

inline SlangEnv* SlangInterpreter::AllocateEnv(size_t size){
	size_t blockCount = size/SLANG_ENV_BLOCK_SIZE;
	if (size%SLANG_ENV_BLOCK_SIZE!=0||blockCount==0) ++blockCount;
	
	SlangEnv* main = nullptr;
	SlangEnv* prev;
	for (size_t i=0;i<blockCount;++i){
		SlangEnv* obj = (SlangEnv*)Allocate(sizeof(SlangEnv));
		obj->header.type = SlangType::Env;
		obj->header.flags = 0;
		obj->header.varCount = 0;
		obj->parent = nullptr;
		obj->next = nullptr;
		if (i==0){
			main = obj;
			StackAddArg((SlangHeader*)main);
		} else {
			prev = (SlangEnv*)funcArgStack[--frameIndex];
			prev->next = obj;
		}
		prev = obj;
		StackAddArg((SlangHeader*)prev);
	}
	--frameIndex;
	return (SlangEnv*)funcArgStack[--frameIndex];
}

inline SlangStorage* SlangInterpreter::AllocateStorage(size_t size,uint16_t elemSize,SlangHeader* fill){
	size_t byteCount = QuantizeSize(size*elemSize);
	SlangStorage* storage = (SlangStorage*)Allocate(sizeof(SlangStorage)+byteCount);
	storage->header.type = SlangType::Storage;
	storage->header.flags = 0;
	storage->header.elemSize = elemSize;
	storage->size = size;
	storage->capacity = byteCount/elemSize;
	
	if (elemSize==sizeof(SlangHeader*)){
		SlangType fillType = GetType(fill);
		SlangHeader** start = storage->objs;
		SlangHeader** end = start+size;
		
		if (fillType==SlangType::List||fillType==SlangType::Vector||
			fillType==SlangType::Lambda||fillType==SlangType::String){
			while (start!=end){
				*start = fill;
				++start;
			}
		} else {
			while (start!=end){
				*start = Copy(fill);
				++start;
			}
		}
	}
	return storage;
}

inline SlangVec* SlangInterpreter::AllocateVec(size_t size,SlangHeader* fill){
	SlangVec* obj = (SlangVec*)Allocate(sizeof(SlangVec));
	obj->header.type = SlangType::Vector;
	obj->header.flags = 0;
	
	if (size==0){
		obj->storage = nullptr;
		return obj;
	}
	
	obj->storage = AllocateStorage(size,sizeof(SlangHeader*),fill);
	return obj;
}

inline SlangStr* SlangInterpreter::AllocateStr(size_t size){
	size_t qsize = QuantizeSize(size);
	SlangStr* obj = (SlangStr*)Allocate(sizeof(SlangStr));
	obj->header.type = SlangType::String;
	obj->header.flags = 0;
	if (size==0){
		obj->storage = nullptr;
		return obj;
	}
	obj->storage = AllocateStorage(qsize,sizeof(uint8_t),nullptr);
	return obj;
}

/*
inline SlangObj* SlangInterpreter::TenureEntireObj(SlangObj** write,SlangObj* obj){
	if (!arena->InCurrSet(obj)) return obj;
	
	SlangObj* toPtr = arena->TenureObj();
	obj->flags |= FLAG_TENURED;
	++gTenureCount;
	
	memcpy(toPtr,obj,sizeof(SlangObj));
	obj->flags |= FLAG_FORWARDED;
	obj->forwarded = toPtr;
	obj->type = (SlangType)37;
	
	
	if (toPtr->type==SlangType::List){
		toPtr->left = TenureEntireObj(write,toPtr->left);
		toPtr->right = TenureEntireObj(write,toPtr->right);
	} else if (toPtr->type==SlangType::Lambda){
		for (auto& [name,envObj] : toPtr->env->symbolMap){
			envObj = TenureEntireObj(write,envObj);
		}
		toPtr->expr = TenureEntireObj(write,toPtr->expr);
	}
	
	return toPtr;
}*/

inline SlangHeader* SlangInterpreter::Evacuate(uint8_t** write,SlangHeader* obj){
	if (!obj) return nullptr;
	if (!arena->InCurrSet((uint8_t*)obj)) return obj;
	
	//uint8_t flags = obj->flags;
	//uint8_t surviveCount = flags&FLAG_SURVIVE_MASK;
	SlangHeader* toPtr;
	
	/*if (surviveCount>=2){
		toPtr = TenureEntireObj(write,obj);
	} else {*/
		//obj->flags = (flags&~FLAG_SURVIVE_MASK) | ((surviveCount+1)&FLAG_SURVIVE_MASK);
		toPtr = (SlangHeader*)*write;
		size_t s = obj->GetSize();
		*write += s;
		memcpy(toPtr,obj,s);
		
		obj->Forward(toPtr);
	//}
	
	return obj->GetForwardAddress();
}
/*
inline void SlangInterpreter::EvacuateEnv(uint8_t** write,SlangEnv* env){
	//env->marked = gcParity;
	for (size_t i=0;i<env.size;++i){
		SlangHeader* obj = env.mappings[i*2+1];
		if (obj&&obj->IsForwarded()){
			obj = obj->GetForwardAddress();
		} else {
			obj = Evacuate(write,obj);
		}
	}
	if (env->parent)
		EvacuateEnv(write,env->parent);
}*/

inline void SlangInterpreter::EvacuateOrForward(uint8_t** write,SlangHeader** obj){
	if (*obj && (*obj)->IsForwarded())
		*obj = (*obj)->GetForwardAddress();
	else
		*obj = Evacuate(write,*obj);
}

inline void SlangInterpreter::ScavengeObj(uint8_t** write,SlangHeader* read){
	switch (read->type){
		case SlangType::List: {
			SlangList* list = (SlangList*)read;
			EvacuateOrForward(write,&list->left);
			EvacuateOrForward(write,&list->right);
			return;
		}
		case SlangType::Lambda: {
			SlangLambda* lam = (SlangLambda*)read;
			EvacuateOrForward(write,&lam->expr);
			EvacuateOrForward(write,(SlangHeader**)&lam->params);
			EvacuateOrForward(write,(SlangHeader**)&lam->env);
			return;
		}
		case SlangType::Vector: {
			SlangVec* vec = (SlangVec*)read;
			EvacuateOrForward(write,(SlangHeader**)&vec->storage);
			
			SlangStorage* storage = vec->storage;
			if (storage){
				for (size_t i=0;i<storage->size;++i){
					EvacuateOrForward(write,(SlangHeader**)&storage->objs[i]);
				}
			}
			return;
		}
		case SlangType::Env: {
			SlangEnv* env = (SlangEnv*)read;
			for (size_t i=0;i<env->header.varCount;++i){
				EvacuateOrForward(write,&env->mappings[i].obj);
			}
			EvacuateOrForward(write,(SlangHeader**)&env->next);
			EvacuateOrForward(write,(SlangHeader**)&env->parent);
			return;
		}
		case SlangType::String: {
			SlangStr* str = (SlangStr*)read;
			EvacuateOrForward(write,(SlangHeader**)&str->storage);
			return;
		}
		
		case SlangType::Params:
		case SlangType::Int:
		case SlangType::Real:
		case SlangType::Bool:
		case SlangType::Symbol:
		case SlangType::Storage:
		case SlangType::NullType:
			return;
	}
	assert(false);
}

inline void SlangInterpreter::ForwardObj(SlangHeader* obj){
	switch (obj->type){
		case SlangType::List: {
			SlangList* list = (SlangList*)obj;
			if (list->left && list->left->IsForwarded())
				list->left = list->left->GetForwardAddress();
			if (list->right && list->right->IsForwarded())
				list->right = list->right->GetForwardAddress();
			return;
		}
		case SlangType::Lambda: {
			SlangLambda* lam = (SlangLambda*)obj;
			if (lam->expr && lam->expr->IsForwarded())
				lam->expr = lam->expr->GetForwardAddress();
			if (lam->params && lam->params->header.IsForwarded())
				lam->params = (SlangParams*)lam->params->header.GetForwardAddress();
			if (lam->env && lam->env->header.IsForwarded())
				lam->env = (SlangEnv*)lam->env->header.GetForwardAddress();
			return;
		}
		case SlangType::Vector: {
			SlangVec* vec = (SlangVec*)obj;
			if (vec->storage && vec->storage->header.IsForwarded())
				vec->storage = (SlangStorage*)vec->storage->header.GetForwardAddress();
			
			SlangStorage* storage = vec->storage;
			for (size_t i=0;i<storage->size;++i){
				if (storage->objs[i] && storage->objs[i]->IsForwarded())
					storage->objs[i] = storage->objs[i]->GetForwardAddress();
			}
			return;
		}
		case SlangType::Env: {
			SlangEnv* env = (SlangEnv*)obj;
			for (size_t i=0;i<env->header.varCount;++i){
				SlangHeader* obj = env->mappings[i].obj;
				if (obj && obj->IsForwarded())
					env->mappings[i].obj = obj->GetForwardAddress();
			}
			if (env->next && env->next->header.IsForwarded())
				env->next = (SlangEnv*)env->next->header.GetForwardAddress();
			if (env->parent && env->parent->header.IsForwarded())
				env->parent = (SlangEnv*)env->parent->header.GetForwardAddress();
			return;
		}
		case SlangType::String: {
			SlangStr* str = (SlangStr*)obj;
			if (str->storage && str->storage->header.IsForwarded())
				str->storage = (SlangStorage*)str->storage->header.GetForwardAddress();
			return;
		}
		
		case SlangType::Params:
		case SlangType::Storage:
		case SlangType::Int:
		case SlangType::Real:
		case SlangType::Bool:
		case SlangType::Symbol:
		case SlangType::NullType:
			return;
	}
	assert(false);
}
/*
inline void SlangInterpreter::ScavengeTenure(SlangObj** write,SlangObj* start,SlangObj* end){
	while (start!=end){
		ScavengeObj(write,start);
		++start;
	}
}*/

inline void SlangInterpreter::Scavenge(uint8_t** write,uint8_t* read){
	while (read<*write){
		SlangHeader* obj = (SlangHeader*)read;
		ScavengeObj(write,obj);
		read += obj->GetSize();
	}
}

inline void SlangInterpreter::ReallocSet(size_t newSize){
	newSize = QuantizeSize16(newSize);
	uint8_t* newPtr = (uint8_t*)malloc(newSize);
	// copy over data
	size_t currPointerLen = arena->currPointer-arena->currSet;
	memcpy(newPtr,arena->currSet,currPointerLen);
	uint8_t* newStart = newPtr;
	uint8_t* newCurrPointer = newPtr+currPointerLen;
	uint8_t* start = arena->currSet;
	while (start<arena->currPointer){
		SlangHeader* obj = (SlangHeader*)start;
		size_t s = obj->GetSize();
		memcpy(newStart,start,s);
		obj->Forward((SlangHeader*)newStart);
		start += s;
		newStart += s;
	}
	
	start = newPtr;
	while (start<newCurrPointer){
		SlangHeader* obj = (SlangHeader*)start;
		ForwardObj(obj);
		
		start += obj->GetSize();
	}
	
	for (size_t i=0;i<frameIndex;++i){
		if (funcArgStack[i] && funcArgStack[i]->IsForwarded())
			funcArgStack[i] = funcArgStack[i]->GetForwardAddress();
	}
	for (size_t i=0;i<=envIndex;++i){
		if (envStack[i] && envStack[i]->header.IsForwarded())
			envStack[i] = (SlangEnv*)envStack[i]->header.GetForwardAddress();
	}
	
	free(arena->memSet);
	arena->SetSpace(newPtr,newSize);
	arena->currPointer = newCurrPointer;
	gMaxArenaSize = (newSize > gMaxArenaSize) ?
						newSize : gMaxArenaSize;
	++gReallocCount;
}

inline void SlangInterpreter::SmallGC(size_t allocAttempt){
	gcParity = !gcParity;
	//SlangObj* tenureStart = arena->currTenurePointer;
	uint8_t* write = arena->otherSet;
	uint8_t* read = write;
	for (size_t i=0;i<frameIndex;++i){
		EvacuateOrForward(&write,&funcArgStack[i]);
	}
	
	for (size_t i=0;i<=envIndex;++i){
		EvacuateOrForward(&write,(SlangHeader**)&envStack[i]);
	}
	
	Scavenge(&write,read);
	/*if (!tenureStart) tenureStart = arena->currTenureSet;
	if (!arena->SameTenureSet(tenureStart,arena->currTenurePointer)){
		auto it = arena->tenureSets.rbegin();
		++it;
		SlangObj* lastTenure = *it+LARGE_SET_SIZE;
		ScavengeTenure(&write,tenureStart,lastTenure);
		ScavengeTenure(&write,arena->currTenureSet,arena->currTenurePointer);
	} else {
		ScavengeTenure(&write,tenureStart,arena->currTenurePointer);
	}*/
	
	arena->SwapSets();
	arena->currPointer = write;
	ssize_t spaceLeft = arena->currSet+arena->memSize/2-write;
	// less than 1/4 of the set left
	if (spaceLeft-(ssize_t)allocAttempt<(ssize_t)arena->memSize/8){
		ReallocSet((arena->memSize)*3/2+allocAttempt*2);
	} else if (arena->memSize>SMALL_SET_SIZE*4&&spaceLeft>(ssize_t)arena->memSize*7/16){
		// set more than 7/8 empty
		size_t m = std::max((size_t)(arena->memSize-spaceLeft*2)*3/2,(size_t)SMALL_SET_SIZE*2);
		ReallocSet(m);
	}
	++gSmallGCs;
}

SlangHeader* SlangInterpreter::Copy(SlangHeader* expr){
	if (!expr) return expr;
	
	StackAddArg(expr);
	size_t size = expr->GetSize();
	SlangHeader* copied = (SlangHeader*)Allocate(size);
	if (expr->IsForwarded())
		memcpy(copied,expr->GetForwardAddress(),size);
	else
		memcpy(copied,expr,size);
	--frameIndex;
	
	return copied;
}

inline bool IsIdentifierChar(char c){
	return (c>='A'&&c<='Z') ||
		   (c>='a'&&c<='z') ||
		   (c>='0'&&c<='9') ||
		   c=='_'||c=='-'||c=='?'||
		   c=='!'||c=='@'||c=='&'||
		   c=='*'||c=='+'||c=='/'||
		   c=='$'||c=='%'||c=='^'||
		   c=='~'||c=='='||c=='.';
}

enum SlangGlobalSymbol {
	SLANG_DEFINE = 0,
	SLANG_LAMBDA,
	SLANG_LET,
	SLANG_SET,
	SLANG_IF,
	SLANG_DO,
	SLANG_APPLY,
	SLANG_QUOTE,
	SLANG_NOT,
	SLANG_NEG,
	SLANG_LIST,
	SLANG_VEC,
	SLANG_VEC_ALLOC,
	SLANG_VEC_SIZE,
	SLANG_VEC_GET,
	SLANG_VEC_SET,
	SLANG_PAIR,
	SLANG_LEFT,
	SLANG_RIGHT,
	SLANG_SET_LEFT,
	SLANG_SET_RIGHT,
	SLANG_INC,
	SLANG_DEC,
	SLANG_ADD,
	SLANG_SUB,
	SLANG_MUL,
	SLANG_DIV,
	SLANG_MOD,
	SLANG_PRINT,
	SLANG_ASSERT,
	SLANG_EQ,
	SLANG_IS,
	SLANG_IS_NULL,
	SLANG_IS_INT,
	SLANG_IS_REAL,
	SLANG_IS_NUM,
	SLANG_IS_STRING,
	SLANG_IS_PAIR,
	SLANG_IS_PROC,
	SLANG_IS_VECTOR,
	SLANG_IS_BOUND,
	GLOBAL_SYMBOL_COUNT
};

SymbolNameDict gDefaultNameDict = {
	{"def",SLANG_DEFINE},
	{"&",SLANG_LAMBDA},
	{"let",SLANG_LET},
	{"set!",SLANG_SET},
	{"if",SLANG_IF},
	{"do",SLANG_DO},
	{"apply",SLANG_APPLY},
	{"quote",SLANG_QUOTE},
	{"not",SLANG_NOT},
	{"neg",SLANG_NEG},
	{"list",SLANG_LIST},
	{"vec",SLANG_VEC},
	{"vec-alloc",SLANG_VEC_ALLOC},
	{"vec-size",SLANG_VEC_SIZE},
	{"vec-get",SLANG_VEC_GET},
	{"vec-set!",SLANG_VEC_SET},
	{"pair",SLANG_PAIR},
	{"L",SLANG_LEFT},
	{"R",SLANG_RIGHT},
	{"setL!",SLANG_SET_LEFT},
	{"setR!",SLANG_SET_RIGHT},
	{"++",SLANG_INC},
	{"--",SLANG_DEC},
	{"+",SLANG_ADD},
	{"-",SLANG_SUB},
	{"*",SLANG_MUL},
	{"/",SLANG_DIV},
	{"%",SLANG_MOD},
	{"print",SLANG_PRINT},
	{"assert",SLANG_ASSERT},
	{"=",SLANG_EQ},
	{"is",SLANG_IS},
	{"null?",SLANG_IS_NULL},
	{"int?",SLANG_IS_INT},
	{"real?",SLANG_IS_REAL},
	{"num?",SLANG_IS_NUM},
	{"str?",SLANG_IS_STRING},
	{"pair?",SLANG_IS_PAIR},
	{"proc?",SLANG_IS_PROC},
	{"vec?",SLANG_IS_VECTOR},
	{"bound?",SLANG_IS_BOUND}
};

inline void SlangEnv::AddBlock(SlangEnv* newEnv){
	newEnv->parent = nullptr;
	SlangEnv* curr = this;
	while (curr->next){
		curr = curr->next;
	}
	curr->next = newEnv;
}

inline bool SlangEnv::DefSymbol(SymbolName name,SlangHeader* obj){
	size_t size = header.varCount;
	if (size==SLANG_ENV_BLOCK_SIZE){
		if (!next) return false;
		
		return next->DefSymbol(name,obj);
	}
	
	mappings[size].sym = name;
	mappings[size].obj = obj;
	++header.varCount;
	return true;
}

inline bool SlangEnv::SetSymbol(SymbolName name,SlangHeader* obj){
	for (size_t i=0;i<header.varCount;++i){
		if (mappings[i].sym==name){
			mappings[i].obj = obj;
			return true;
		}
	}
	if (!next) return false;
	return next->SetSymbol(name,obj);
}

inline bool SlangEnv::GetSymbol(SymbolName name,SlangHeader** res){
	for (size_t i=0;i<header.varCount;++i){
		if (mappings[i].sym==name){
			*res = mappings[i].obj;
			return true;
		}
	}
	if (next){
		return next->GetSymbol(name,res);
	}
	
	return false;
}

inline SymbolName SlangInterpreter::GenSym(){
	return (genIndex++) | GEN_FLAG;
}

inline SlangObj* SlangInterpreter::MakeInt(int64_t i){
	SlangObj* obj = AllocateSmallObj();
	obj->header.type = SlangType::Int;
	obj->integer = i;
	return obj;
}

inline SlangObj* SlangInterpreter::MakeReal(double r){
	SlangObj* obj = AllocateSmallObj();
	obj->header.type = SlangType::Real;
	obj->real = r;
	return obj;
}

inline SlangObj* SlangInterpreter::MakeBool(bool b){
	SlangObj* obj = AllocateSmallObj();
	obj->header.type = SlangType::Bool;
	obj->boolean = b;
	return obj;
}

inline SlangObj* SlangInterpreter::MakeSymbol(SymbolName symbol){
	SlangObj* obj = AllocateSmallObj();
	obj->header.type = SlangType::Symbol;
	obj->symbol = symbol;
	return obj;
}

inline SlangParams* SlangInterpreter::MakeParams(const std::vector<SymbolName>& args){
	SlangParams* params = AllocateParams(args.size());
	size_t i=0;
	for (const auto& arg : args){
		params->params[i++] = arg;
	}
	return params;
}

inline SlangLambda* SlangInterpreter::MakeLambda(
				SlangHeader* expr,
				const std::vector<SymbolName>& args,
				bool variadic,
				bool extraSlot){
	SlangLambda* obj = AllocateLambda();
	if (variadic)
		obj->header.flags |= FLAG_VARIADIC;
	obj->expr = expr;
	obj->env = nullptr;
	obj->params = nullptr;
	funcArgStack[frameIndex++] = (SlangHeader*)obj;
	
	SlangParams* params = MakeParams(args);
	((SlangLambda*)funcArgStack[frameIndex-1])->params = params;
	
	size_t allocCount = args.size();
	if (extraSlot) ++allocCount;
	SlangEnv* lamEnv = AllocateEnv(allocCount);
	
	obj = (SlangLambda*)funcArgStack[--frameIndex];
	obj->env = lamEnv;
	obj->env->parent = GetCurrEnv();
	
	for (const auto& arg : args){
		bool ret = obj->env->DefSymbol(arg,nullptr);
		assert(ret);
	}
	
	return obj;
}

inline SlangVec* SlangInterpreter::MakeVec(const std::vector<SlangHeader*>& objs,size_t start,size_t end){
	size_t m = std::min(end,objs.size());
	size_t size = m-start;
	SlangVec* vecObj = AllocateVec(size);
	for (size_t i=0;i<size;++i){
		vecObj->storage->objs[i] = objs[start+i];
	}
	return vecObj;
}

inline SlangList* SlangInterpreter::MakeList(const std::vector<SlangHeader*>& objs,size_t start,size_t end){
	SlangList* listHead = nullptr;
	SlangList* list = nullptr;
	SlangList* prev = nullptr;
	size_t m = std::min(end,objs.size());
	for (size_t i=start;i<m;++i){
		list = AllocateList();
		list->left = objs[i];
		list->right = nullptr;
		
		if (i==start){
			listHead = list;
			StackAddArg((SlangHeader*)listHead);
		} else {
			prev = (SlangList*)funcArgStack[--frameIndex];
			prev->right = (SlangHeader*)list;
		}
		
		prev = list;
		StackAddArg((SlangHeader*)prev);
	}
	--frameIndex;
	listHead = (SlangList*)funcArgStack[--frameIndex];
	
	return listHead;
}

bool GCManualCollect(SlangInterpreter* s,SlangList* args,SlangHeader** res){
	if (args!=nullptr) return false;
	s->SmallGC(0);
	*res = nullptr;
	return true;
}

bool GCMemSize(SlangInterpreter* s,SlangList* args,SlangHeader** res){
	if (args!=nullptr) return false;
	size_t size = s->arena->currPointer - s->arena->currSet;
	*res = (SlangHeader*)s->MakeInt(size);
	return true;
}

bool GCMemCapacity(SlangInterpreter* s,SlangList* args,SlangHeader** res){
	if (args!=nullptr) return false;
	size_t size = s->arena->memSize/2;
	*res = (SlangHeader*)s->MakeInt(size);
	return true;
}

inline bool IsPredefinedSymbol(SymbolName name){
	if (name<GLOBAL_SYMBOL_COUNT){
		return true;
	}
	return false;
}

inline bool GetRecursiveSymbol(SlangEnv* e,SymbolName name,SlangHeader** val){
	while (e){
		if (e->GetSymbol(name,val))
			return true;
		e = e->parent;
	}
	return false;

}
inline void SlangInterpreter::DefEnvSymbol(SymbolName name,SlangHeader* val){
	SlangEnv* e = GetCurrEnv();
	StackAddArg(val);
	if (!e->DefSymbol(name,val)){
		SlangEnv* newEnv = AllocateEnv(1);
		if (!newEnv->DefSymbol(name,funcArgStack[--frameIndex])){
			std::cout << "unreachable\n";
			exit(1);
		}
		e = GetCurrEnv();
		e->AddBlock(newEnv);
	}
}

inline bool SlangInterpreter::SetRecursiveSymbol(SlangEnv* e,SymbolName name,SlangHeader* val){
	SlangHeader* t;
	while (e){
		if (e->GetSymbol(name,&t)){
			return e->SetSymbol(name,val);
		}
		e = e->parent;
	}
	return false;
}

#define NEXT_ARG(x) { \
	if (!NextArg(x)){ \
		EvalError((SlangHeader*)x); \
		return false; \
	}} \

inline bool SlangInterpreter::SlangFuncDefine(SlangList* argIt,SlangHeader** res){
	// can't exist
	if (GetType(argIt->left)!=SlangType::Symbol){
		TypeError(argIt->left,GetType(argIt->left),SlangType::Symbol);
		return false;
	}
	SlangObj* symObj = (SlangObj*)argIt->left;
	SymbolName sym = symObj->symbol;
	SlangHeader* tmp;
	if (GetCurrEnv()->GetSymbol(sym,&tmp)){
		RedefinedError((SlangHeader*)argIt,sym);
		return false;
	}
	
	NEXT_ARG(&argIt);
	
	SlangHeader* valObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&valObj))
		return false;
	
	DefEnvSymbol(sym,valObj);
	*res = valObj;
	return true;
}

inline bool SlangInterpreter::SlangFuncLambda(SlangList* argIt,SlangHeader** res){
	if (!IsList(argIt->left)&&GetType(argIt->left)!=SlangType::Symbol){
		std::stringstream msg = {};
		msg << "LambdaError:\n    Expected parameter list, not '" << *argIt->left << "'";
		PushError(argIt->left,msg.str());
		return false;
	}
	
	bool variadic = GetType(argIt->left)==SlangType::Symbol;
	
	std::vector<SymbolName> params = {};
	params.reserve(5);
	if (!variadic){
		std::set<SymbolName> seen = {};
		SlangList* paramIt = (SlangList*)argIt->left;
		while (paramIt){
			if (paramIt->header.type!=SlangType::List){
				TypeError((SlangHeader*)paramIt,paramIt->header.type,SlangType::List);
				return false;
			}
			if (GetType(paramIt->left)!=SlangType::Symbol){
				TypeError(paramIt->left,GetType(paramIt->left),SlangType::Symbol);
				return false;
			}
			SlangObj* symObj = (SlangObj*)paramIt->left;
			if (seen.contains(symObj->symbol)){
				std::stringstream msg = {};
				msg << "LambdaError:\n    Lambda parameter '" << 
					*paramIt->left << "' was reused!";
				PushError((SlangHeader*)symObj,msg.str());
				return false;
			}
			
			params.push_back(symObj->symbol);
			seen.insert(symObj->symbol);
			
			NEXT_ARG(&paramIt);
		}
	} else { // variadic
		params.push_back(((SlangObj*)argIt->left)->symbol);
	}
	NEXT_ARG(&argIt);

	SlangLambda* lambdaObj = MakeLambda(argIt->left,params,variadic);
	*res = (SlangHeader*)lambdaObj;
	return true;
}

inline bool SlangInterpreter::SlangFuncSet(SlangList* argIt,SlangHeader** res){
	if (GetType(argIt->left)!=SlangType::Symbol){
		TypeError(argIt->left,GetType(argIt->left),SlangType::Symbol);
		return false;
	}
	
	SymbolName sym = ((SlangObj*)argIt->left)->symbol;
	NEXT_ARG(&argIt);
	
	SlangHeader* valObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&valObj))
		return false;
		
	if (!SetRecursiveSymbol(GetCurrEnv(),sym,valObj)){
		std::stringstream msg = {};
		msg << "SetError:\n    Cannot set undefined symbol '" << *argIt->left << "'";
		PushError(argIt->left,msg.str());
		return false;
	}
	
	*res = valObj;
	return true;
}

inline bool SlangInterpreter::SlangFuncQuote(SlangList* argIt,SlangHeader** res){
	//if (argIt->left){
		*res = Copy(argIt->left);
	//} else {
	//	*res = nullptr;
	//}
	return true;
}

inline bool ConvertToBool(const SlangHeader* obj,bool& res){
	if (!obj){
		res = false;
		return true;
	}
	
	switch (obj->type){
		case SlangType::NullType:
		case SlangType::Storage:
			return false;
		case SlangType::Bool:
			res = ((SlangObj*)obj)->boolean;
			return true;
		case SlangType::Int:
			res = (bool)((SlangObj*)obj)->integer;
			return true;
		case SlangType::Real:
			res = (bool)((SlangObj*)obj)->real;
			return true;
			res = true;
			return true;
		case SlangType::Vector: {
			SlangVec* vec = (SlangVec*)obj;
			if (!vec->storage){
				res = false;
			} else {
				res = vec->storage->size!=0;
			}
			return true;
		}
		case SlangType::String: {
			SlangStr* str = (SlangStr*)obj;
			if (!str->storage){
				res = false;
			} else {
				res = str->storage->size!=0;
			}
			return true;
		}
		case SlangType::Params:
			res = ((SlangParams*)obj)->size!=0;
			return true;
		case SlangType::Env:
		case SlangType::Lambda:
		case SlangType::List:
		case SlangType::Symbol:
			res = true;
			return true;
	}
	assert(0);
	return false;
}

inline bool SlangInterpreter::SlangFuncNot(SlangList* argIt,SlangHeader** res){
	SlangHeader* boolObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&boolObj))
		return false;
	
	bool b;
	if (!ConvertToBool(boolObj,b)){
		TypeError(boolObj,GetType(boolObj),SlangType::Bool);
		return false;
	}
	
	*res = (SlangHeader*)MakeBool(!b);
	return true;
}

inline bool SlangInterpreter::SlangFuncNegate(SlangList* argIt,SlangHeader** res){
	SlangHeader* numObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&numObj))
		return false;
	
	if (!numObj){
		TypeError(numObj,SlangType::NullType,SlangType::Int);
		return false;
	}
	
	switch (numObj->type){
		case SlangType::Int:
			((SlangObj*)numObj)->integer = -(((SlangObj*)numObj)->integer);
			*res = numObj;
			return true;
		case SlangType::Real:
			((SlangObj*)numObj)->real = -(((SlangObj*)numObj)->real);
			*res = numObj;
			return true;
		default:
			TypeError(numObj,numObj->type,SlangType::Int);
			return false;
	}
}

inline bool SlangInterpreter::SlangFuncList(SlangList* argIt,SlangHeader** res){
	if (!argIt){
		*res = nullptr;
		return true;
	}
	
	size_t frameStart = frameIndex;
	SlangHeader* leval;
	while (argIt->right){
		if (!WrappedEvalExpr(argIt->left,&leval))
			return false;
		
		StackAddArg(leval);
		NEXT_ARG(&argIt);
	}
	// last elem
	if (!WrappedEvalExpr(argIt->left,&leval))
		return false;
	
	StackAddArg(leval);
	
	SlangList* listObj = MakeList(funcArgStack,frameStart,frameIndex);
	*res = (SlangHeader*)listObj;
	frameIndex = frameStart;
	return true;
}

inline bool SlangInterpreter::SlangFuncVec(SlangList* argIt,SlangHeader** res){
	if (!argIt){
		*res = (SlangHeader*)AllocateVec(0);
		return true;
	}
	
	size_t frameStart = frameIndex;
	SlangHeader* leval;
	while (argIt->right){
		if (!WrappedEvalExpr(argIt->left,&leval))
			return false;
		
		StackAddArg(leval);
		NEXT_ARG(&argIt);
	}
	// last elem
	if (!WrappedEvalExpr(argIt->left,&leval))
		return false;
	
	StackAddArg(leval);
	
	SlangVec* vecObj = MakeVec(funcArgStack,frameStart,frameIndex);
	*res = (SlangHeader*)vecObj;
	frameIndex = frameStart;
	return true;
}

inline bool SlangInterpreter::SlangFuncVecAlloc(SlangList* argIt,SlangHeader** res){
	SlangHeader* sizeObj;
	if (!WrappedEvalExpr(argIt->left,&sizeObj))
		return false;
	
	if (GetType(sizeObj)!=SlangType::Int){
		TypeError(sizeObj,GetType(sizeObj),SlangType::Int);
		return false;
	}
	size_t size = ((SlangObj*)sizeObj)->integer;
	
	SlangHeader* fill = nullptr;
	if (argIt->right){
		NEXT_ARG(&argIt);
		if (!WrappedEvalExpr(argIt->left,&fill))
			return false;
	}
	
	SlangVec* vecObj = AllocateVec(size,fill);
	*res = (SlangHeader*)vecObj;
	return true;
}

inline bool SlangInterpreter::SlangFuncVecSize(SlangList* argIt,SlangHeader** res){
	SlangHeader* vecObj;
	if (!WrappedEvalExpr(argIt->left,&vecObj))
		return false;
	
	if (GetType(vecObj)!=SlangType::Vector){
		TypeError(vecObj,GetType(vecObj),SlangType::Vector);
		return false;
	}
	
	SlangVec* vec = (SlangVec*)vecObj;
	
	*res = (SlangHeader*)MakeInt(vec->GetLength());
	return true;
}

inline bool SlangInterpreter::SlangFuncVecGet(SlangList* argIt,SlangHeader** res){
	SlangHeader* vecObj;
	if (!WrappedEvalExpr(argIt->left,&vecObj))
		return false;
	
	if (GetType(vecObj)!=SlangType::Vector){
		TypeError(vecObj,GetType(vecObj),SlangType::Vector);
		return false;
	}
	
	SlangVec* vec = (SlangVec*)vecObj;
	
	NEXT_ARG(&argIt);

	SlangHeader* indexObj;
	if (!WrappedEvalExpr(argIt->left,&indexObj))
		return false;
	
	if (GetType(indexObj)!=SlangType::Int){
		TypeError(indexObj,GetType(indexObj),SlangType::Int);
		return false;
	}
	ssize_t index = ((SlangObj*)indexObj)->integer;
	size_t vecLen = vec->GetLength();
	if (index<0) index += vecLen;
	if (index>=(ssize_t)vecLen||index<0){
		if (index<0) index -= vecLen;
		IndexError(argIt->left,vecLen,index);
		return false;
	}
	
	*res = vec->storage->objs[index];
	return true;
}

inline bool SlangInterpreter::SlangFuncVecSet(SlangList* argIt,SlangHeader** res){
	SlangHeader* vecObj;
	if (!WrappedEvalExpr(argIt->left,&vecObj))
		return false;
	
	if (GetType(vecObj)!=SlangType::Vector){
		TypeError(vecObj,GetType(vecObj),SlangType::Vector);
		return false;
	}
	
	SlangVec* vec = (SlangVec*)vecObj;
	StackAddArg((SlangHeader*)vec);
	
	NEXT_ARG(&argIt);

	SlangHeader* indexObj;
	if (!WrappedEvalExpr(argIt->left,&indexObj))
		return false;
	
	if (GetType(indexObj)!=SlangType::Int){
		TypeError(indexObj,GetType(indexObj),SlangType::Int);
		return false;
	}
	ssize_t index = ((SlangObj*)indexObj)->integer;
	size_t vecLen = vec->GetLength();
	if (index<0) index += vecLen;
	if (index>=(ssize_t)vecLen||index<0){
		if (index<0) index -= vecLen;
		IndexError(argIt->left,vecLen,index);
		return false;
	}
	
	NEXT_ARG(&argIt);
	SlangHeader* val;
	if (!WrappedEvalExpr(argIt->left,&val))
		return false;
	
	vec = (SlangVec*)funcArgStack[--frameIndex];
	vec->storage->objs[index] = val;
	*res = val;
	return true;
}

inline bool SlangInterpreter::SlangFuncPair(SlangList* argIt,SlangHeader** res){
	SlangList* pairObj = AllocateList();
	// for GC
	StackAddArg((SlangHeader*)pairObj);
	
	pairObj->left = nullptr;
	pairObj->right = nullptr;
	
	SlangHeader* left = nullptr;
	if (!WrappedEvalExpr(argIt->left,&left))
		return false;
		
	((SlangList*)funcArgStack[frameIndex-1])->left = left;
	
	NEXT_ARG(&argIt);
	
	SlangHeader* right = nullptr;
	if (!WrappedEvalExpr(argIt->left,&right))
		return false;
	
	((SlangList*)funcArgStack[frameIndex-1])->right = right;
	
	*res = funcArgStack[--frameIndex];
	return true;
}

inline bool SlangInterpreter::SlangFuncLeft(SlangList* argIt,SlangHeader** res){
	SlangHeader* pairObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&pairObj))
		return false;
		
	if (GetType(pairObj)!=SlangType::List){
		TypeError(pairObj,GetType(pairObj),SlangType::List);
		return false;
	}
	
	SlangList* pair = (SlangList*)pairObj;
	*res = pair->left;
	return true;
}

inline bool SlangInterpreter::SlangFuncRight(SlangList* argIt,SlangHeader** res){
	SlangHeader* pairObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&pairObj))
		return false;
	
	if (GetType(pairObj)!=SlangType::List){
		TypeError(pairObj,GetType(pairObj),SlangType::List);
		return false;
	}
	
	SlangList* pair = (SlangList*)pairObj;
	*res = pair->right;
	return true;
}

inline bool SlangInterpreter::SlangFuncSetLeft(SlangList* argIt,SlangHeader** res){
	SlangHeader* pairObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&pairObj))
		return false;
		
	if (GetType(pairObj)!=SlangType::List){
		TypeError(pairObj,GetType(pairObj),SlangType::List);
		return false;
	}
	
	SlangList* pair = (SlangList*)pairObj;
	
	NEXT_ARG(&argIt);
	
	SlangHeader* valObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&valObj))
		return false;
	
	pair->left = valObj;
	*res = (SlangHeader*)pair;
	return true;
}

inline bool SlangInterpreter::SlangFuncSetRight(SlangList* argIt,SlangHeader** res){
	SlangHeader* pairObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&pairObj))
		return false;
	
	if (GetType(pairObj)!=SlangType::List){
		TypeError(pairObj,GetType(pairObj),SlangType::List);
		return false;
	}
	SlangList* pair = (SlangList*)pairObj;
	
	NEXT_ARG(&argIt);
	
	SlangHeader* valObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&valObj))
		return false;
	
	pair->right = valObj;
	*res = (SlangHeader*)pair;
	return true;
}

inline bool SlangInterpreter::SlangFuncInc(SlangList* argIt,SlangHeader** res){
	SlangHeader* numObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&numObj))
		return false;
	
	if (GetType(numObj)!=SlangType::Int){
		TypeError(numObj,GetType(numObj),SlangType::Int);
		return false;
	}
	
	*res = (SlangHeader*)MakeInt(((SlangObj*)numObj)->integer+1);
	return true;
}

inline bool SlangInterpreter::SlangFuncDec(SlangList* argIt,SlangHeader** res){
	SlangHeader* numObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&numObj))
		return false;
	
	if (GetType(numObj)!=SlangType::Int){
		TypeError(numObj,GetType(numObj),SlangType::Int);
		return false;
	}
	
	*res = (SlangHeader*)MakeInt(((SlangObj*)numObj)->integer-1);
	return true;
}

inline bool SlangInterpreter::AddReals(SlangList* argIt,SlangHeader** res,double curr){
	while (argIt){
		SlangHeader* valObj = nullptr;
		if (!WrappedEvalExpr(argIt->left,&valObj))
			return false;
		
		if (GetType(valObj)==SlangType::Real){
			curr += ((SlangObj*)valObj)->real;
		} else if (GetType(valObj)==SlangType::Int){
			curr += ((SlangObj*)valObj)->integer;
		} else {
			TypeError(valObj,GetType(valObj),SlangType::Real);
			return false;
		}
		
		NEXT_ARG(&argIt);
	}
	
	*res = (SlangHeader*)MakeReal(curr);
	return true;
}

inline bool SlangInterpreter::SlangFuncAdd(SlangList* argIt,SlangHeader** res){
	int64_t sum = 0;
	size_t argCount = 0;
	while (argIt){
		SlangHeader* valObj = nullptr;
		if (!WrappedEvalExpr(argIt->left,&valObj))
			return false;
		
		if (GetType(valObj)==SlangType::Int){
			sum += ((SlangObj*)valObj)->integer;
		} else if (GetType(valObj)==SlangType::Real){
			NEXT_ARG(&argIt);
			return AddReals(argIt,res,((SlangObj*)valObj)->real+(double)sum);
		} else {
			TypeError(valObj,GetType(valObj),SlangType::Int,argCount+1);
			return false;
		}
		
		++argCount;
		NEXT_ARG(&argIt);
	}
	
	*res = (SlangHeader*)MakeInt(sum);
	return true;
}

inline bool SlangInterpreter::SlangFuncSub(SlangList* argIt,SlangHeader** res){
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
	
	if (!IsNumeric(l)){
		TypeError(l,GetType(l),SlangType::Int);
		return false;
	}
	StackAddArg(l);
	
	NEXT_ARG(&argIt);
	SlangHeader* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r))
		return false;
		
	if (!IsNumeric(r)){
		TypeError(r,GetType(r),SlangType::Int);
		return false;
	}
	
	l = funcArgStack[--frameIndex];
	
	SlangObj* lobj = (SlangObj*)l;
	SlangObj* robj = (SlangObj*)r;
	if (l->type==SlangType::Int){
		if (r->type==SlangType::Int){
			*res = (SlangHeader*)MakeInt(lobj->integer - robj->integer);
			return true;
		} else {
			*res = (SlangHeader*)MakeReal((double)lobj->integer - robj->real);
			return true;
		}
	} else {
		if (r->type==SlangType::Int){
			*res = (SlangHeader*)MakeReal(lobj->real - (double)robj->integer);
			return true;
		} else {
			*res = (SlangHeader*)MakeReal(lobj->real - robj->real);
			return true;
		}
	}
}

inline bool SlangInterpreter::MulReals(SlangList* argIt,SlangHeader** res,double curr){
	while (argIt){
		SlangHeader* valObj = nullptr;
		if (!WrappedEvalExpr(argIt->left,&valObj))
			return false;
		
		if (GetType(valObj)==SlangType::Real){
			curr *= ((SlangObj*)valObj)->real;
		} else if (GetType(valObj)==SlangType::Int){
			curr *= ((SlangObj*)valObj)->integer;
		} else {
			TypeError(valObj,GetType(valObj),SlangType::Real);
			return false;
		}
		
		NEXT_ARG(&argIt);
	}
	
	*res = (SlangHeader*)MakeReal(curr);
	return true;
}

inline bool SlangInterpreter::SlangFuncMul(SlangList* argIt,SlangHeader** res){
	int64_t prod = 1;
	size_t argCount = 0;
	while (argIt){
		SlangHeader* valObj = nullptr;
		if (!WrappedEvalExpr(argIt->left,&valObj))
			return false;
			
		if (GetType(valObj)==SlangType::Int){
			prod *= ((SlangObj*)valObj)->integer;
		} else if (GetType(valObj)==SlangType::Real){
			NEXT_ARG(&argIt);
			return MulReals(argIt,res,(double)prod*((SlangObj*)valObj)->real);
		} else {
			TypeError(valObj,GetType(valObj),SlangType::Int,argCount+1);
			return false;
		}
		
		NEXT_ARG(&argIt);
		++argCount;
	}
	
	*res = (SlangHeader*)MakeInt(prod);
	return true;
}

inline int64_t floordiv(int64_t a,int64_t b){
	int64_t d = a/b;
	int64_t r = a%b;
	return r ? (d-((a<0)^(b<0))) : d;
}

inline int64_t floormod(int64_t a,int64_t b){
	return a-floordiv(a,b)*b;
}

inline bool SlangInterpreter::SlangFuncDiv(SlangList* argIt,SlangHeader** res){
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
		
	if (!IsNumeric(l)){
		TypeError(l,GetType(l),SlangType::Int);
		return false;
	}
	
	NEXT_ARG(&argIt);
	
	SlangHeader* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r))
		return false;
		
	if (!IsNumeric(r)){
		TypeError(r,GetType(r),SlangType::Int);
		return false;
	}
	
	SlangObj* lobj = (SlangObj*)l;
	SlangObj* robj = (SlangObj*)r;
	
	if (l->type==SlangType::Int){
		if (r->type==SlangType::Int){
			if (robj->integer==0){
				ZeroDivisionError((SlangHeader*)argIt);
				return false;
			}
			*res = (SlangHeader*)MakeInt(floordiv(lobj->integer,robj->integer));
			return true;
		} else {
			if (robj->real==0.0){
				ZeroDivisionError((SlangHeader*)argIt);
				return false;
			}
			*res = (SlangHeader*)MakeReal((double)lobj->integer/robj->real);
			return true;
		}
	} else {
		if (r->type==SlangType::Int){
			if (robj->integer==0){
				ZeroDivisionError((SlangHeader*)argIt);
				return false;
			}
			*res = (SlangHeader*)MakeReal(lobj->real/(double)robj->integer);
			return true;
		} else {
			if (robj->real==0.0){
				ZeroDivisionError((SlangHeader*)argIt);
				return false;
			}
			*res = (SlangHeader*)MakeReal(lobj->real/robj->real);
			return true;
		}
	}
}

inline bool SlangInterpreter::SlangFuncMod(SlangList* argIt,SlangHeader** res){
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
		
	if (!IsNumeric(l)){
		TypeError(l,GetType(l),SlangType::Int);
		return false;
	}
	
	NEXT_ARG(&argIt);
	SlangHeader* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r))
		return false;
		
	if (!IsNumeric(r)){
		TypeError(r,GetType(r),SlangType::Int);
		return false;
	}
	
	SlangObj* lobj = (SlangObj*)l;
	SlangObj* robj = (SlangObj*)r;
	
	if (l->type==SlangType::Int){
		if (r->type==SlangType::Int){
			if (robj->integer==0){
				ZeroDivisionError((SlangHeader*)argIt);
				return false;
			}
			*res = (SlangHeader*)MakeInt(floormod(lobj->integer,robj->integer));
			return true;
		} else {
			if (robj->real==0.0){
				ZeroDivisionError((SlangHeader*)argIt);
				return false;
			}
			*res = (SlangHeader*)MakeReal(fmod((double)lobj->integer,robj->real));
			return true;
		}
	} else {
		if (r->type==SlangType::Int){
			if (robj->integer==0){
				ZeroDivisionError((SlangHeader*)argIt);
				return false;
			}
			*res = (SlangHeader*)MakeReal(fmod(lobj->real,(double)robj->integer));
			return true;
		} else {
			if (robj->real==0.0){
				ZeroDivisionError((SlangHeader*)argIt);
				return false;
			}
			*res = (SlangHeader*)MakeReal(fmod(lobj->real,robj->real));
			return true;
		}
	}
}

inline bool SlangInterpreter::SlangFuncPrint(SlangList* argIt,SlangHeader** res){
	SlangHeader* printObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&printObj)){
		EvalError(argIt->left);
		return false;
	}
	
	if (printObj){
		std::cout << *printObj << '\n';
	} else {
		std::cout << "()\n";
	}
	*res = printObj;
	return true;
}

inline bool SlangInterpreter::SlangFuncAssert(SlangList* argIt,SlangHeader** res){
	SlangHeader* cond = nullptr;
	if (!WrappedEvalExpr(argIt->left,&cond))
		return false;
	
	bool test;
	if (!ConvertToBool(cond,test)){
		TypeError(cond,GetType(cond),SlangType::Bool);
		return false;
	}
	
	if (!test){
		AssertError(argIt->left);
		return false;
	}
	
	*res = cond;
	return true;
}

bool ListEquality(const SlangList* a,const SlangList* b);
bool VectorEquality(const SlangVec* a,const SlangVec* b);
bool StringEquality(const SlangStr* a,const SlangStr* b);

bool EqualObjs(const SlangHeader* a,const SlangHeader* b){
	if (a==b) return true;
	if (!a||!b) return false;
	
	if (a->type==b->type){
		switch (a->type){
			case SlangType::NullType:
			case SlangType::Storage:
			case SlangType::Lambda:
			case SlangType::Env:
			case SlangType::Params:
				return false;
			case SlangType::List:
				return ListEquality((SlangList*)a,(SlangList*)b);
			case SlangType::Vector:
				return VectorEquality((SlangVec*)a,(SlangVec*)b);
			case SlangType::String:
				return StringEquality((SlangStr*)a,(SlangStr*)b);
			case SlangType::Int:
			case SlangType::Real:
			case SlangType::Bool:
			case SlangType::Symbol:
				// union moment
				return ((SlangObj*)a)->integer==((SlangObj*)b)->integer;
		}
	} else {
		if (b->type==SlangType::Int) std::swap(a,b);
		if (a->type==SlangType::Int&&b->type==SlangType::Real){
			return ((double)((SlangObj*)a)->integer)==((SlangObj*)b)->real;
		}
	}
	
	return false;
}

bool ListEquality(const SlangList* a,const SlangList* b){
	if (!EqualObjs(a->left,b->left)) return false;
	if (!EqualObjs(a->right,b->right)) return false;
	
	return true;
}

bool VectorEquality(const SlangVec* a,const SlangVec* b){
	if (!a->storage || !b->storage){
		return a->storage == b->storage;
	}
	
	if (a->storage->size!=b->storage->size) return false;
	for (size_t i=0;i<a->storage->size;++i){
		if (!EqualObjs(a->storage->objs[i],b->storage->objs[i]))
			return false;
	}
	return true;
}

bool StringEquality(const SlangStr* a,const SlangStr* b){
	if (!a->storage || !b->storage){
		return a->storage == b->storage;
	}
	
	if (a->storage->size!=b->storage->size) return false;
	for (size_t i=0;i<a->storage->size;++i){
		if (a->storage->data[i]!=b->storage->data[i])
			return false;
	}
	return true;
}

inline bool SlangInterpreter::SlangFuncEq(SlangList* argIt,SlangHeader** res){
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
	StackAddArg(l);
	
	NEXT_ARG(&argIt);
	SlangHeader* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r))
		return false;
	
	l = funcArgStack[--frameIndex];
	
	*res = (SlangHeader*)MakeBool(EqualObjs(l,r));
	return true;
}

inline bool SlangInterpreter::SlangFuncIs(SlangList* argIt,SlangHeader** res){
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l)){
		EvalError(argIt->left);
		return false;
	}
		
	StackAddArg(l);
	
	SlangHeader* r = nullptr;
	NEXT_ARG(&argIt);
	if (!WrappedEvalExpr(argIt->left,&r)){
		EvalError(argIt->left);
		return false;
	}
		
	l = funcArgStack[--frameIndex];
	
	if (l==r){
		*res = (SlangHeader*)MakeBool(true);
		return true;
	}
	if (!l||!r){
		*res = (SlangHeader*)MakeBool(false);
		return true;
	}
	
	if (l->type!=r->type){
		*res = (SlangHeader*)MakeBool(false);
		return true;
	}
	
	// if both are list type, comparing pointers
	// alone is enough to determine if l IS r
	switch (l->type){
		case SlangType::Vector:
		case SlangType::String:
		case SlangType::List:
		case SlangType::NullType:
		case SlangType::Env:
		case SlangType::Params:
		case SlangType::Lambda:
		case SlangType::Storage:
			*res = (SlangHeader*)MakeBool(false);
			return true;
		case SlangType::Int:
		case SlangType::Real:
		case SlangType::Bool:
		case SlangType::Symbol:
			// union moment
			*res = (SlangHeader*)MakeBool(((SlangObj*)l)->integer==((SlangObj*)r)->integer);
			return true;
	}
	
	*res = (SlangHeader*)MakeBool(false);
	return true;
}

inline void SlangInterpreter::StackAddArg(SlangHeader* arg){
	assert(frameIndex<funcArgStack.size());
	
	gMaxStackHeight = (gMaxStackHeight<frameIndex) ? frameIndex : gMaxStackHeight;
	funcArgStack[frameIndex++] = arg;
}

inline void SlangInterpreter::PushEnv(SlangEnv* env){
	if (envIndex>=envStack.size()-1){
		envStack.resize(envStack.size()*3/2);
	}
	envStack[++envIndex] = env;
}

inline void SlangInterpreter::PopEnv(){
	assert(envIndex!=0);
	--envIndex;
}

inline bool SlangInterpreter::WrappedEvalExpr(SlangHeader* expr,SlangHeader** res){
	++gCurrDepth;
	if (gCurrDepth>gMaxDepth) gMaxDepth = gCurrDepth;
	PushEnv(GetCurrEnv());
	bool b = EvalExpr(expr,res);
	PopEnv();
	--gCurrDepth;
	return b;
}

inline size_t GetArgCount(const SlangList* arg){
	size_t c = 0;
	while (arg){
		++c;
		if (GetType(arg->right)!=SlangType::List) break;
		arg = (SlangList*)arg->right;
	}
	return c;
}

inline SlangEnv* SlangInterpreter::CreateEnv(size_t hint){
	SlangEnv* env = AllocateEnv(hint);
	++gEnvCount;
	++gEnvBalance;
	return env;
}

inline bool SlangInterpreter::GetLetParams(
			std::vector<SymbolName>& params,
			std::vector<SlangHeader*>& exprs,
			SlangList* paramIt){
	std::set<SymbolName> seen{};
	while (paramIt){
		if (GetType(paramIt->left)!=SlangType::List){
			TypeError(paramIt->left,GetType(paramIt->left),SlangType::List);
			return false;
		}
		SlangList* pair = (SlangList*)paramIt->left;
		
		SlangHeader* sym = pair->left;
		if (GetType(sym)!=SlangType::Symbol){
			TypeError(sym,GetType(sym),SlangType::Symbol);
			return false;
		}
		SymbolName s = ((SlangObj*)sym)->symbol;
		if (seen.contains(s)){
			RedefinedError((SlangHeader*)paramIt,s);
			return false;
		}
		params.push_back(s);
		seen.insert(s);
		if (GetType(pair->right)!=SlangType::List){
			TypeError(pair->right,GetType(pair->right),SlangType::List);
			return false;
		}
		pair = (SlangList*)pair->right;
		exprs.push_back(pair->left);
		
		if (pair->right){
			// too many args in init pair
			ArityError(paramIt->left,3,2);
			return false;
		}
		
		NEXT_ARG(&paramIt);
	}
	return true;
}

#define ARITY_EXACT_CHECK(x) \
	if (argCount!=(x)){ \
		ArityError(expr,argCount,(x)); \
		return false; \
	}

bool SlangInterpreter::EvalExpr(SlangHeader* expr,SlangHeader** res){
	++gEvalRecurCounter;
	while (true){
		//currEnv = env;
		if (!expr){
			*res = nullptr;
			return true;
		}
		++gEvalCounter;
		switch (expr->type){
			case SlangType::NullType:
			case SlangType::Real:
			case SlangType::Int:
			case SlangType::Bool:
			case SlangType::String:
			case SlangType::Vector:
			case SlangType::Env:
			case SlangType::Storage:
			case SlangType::Params:
			case SlangType::Lambda: {
				*res = expr;
				return true;
			}
			case SlangType::Symbol: {
				SlangEnv* parent = GetCurrEnv();
				SlangHeader* found = nullptr;
				SymbolName sym = ((SlangObj*)expr)->symbol;
				if (GetRecursiveSymbol(parent,sym,&found)){
					*res = found;
					return true;
				}
				
				if (IsPredefinedSymbol(sym)){
					*res = expr;
					return true;
				}
				
				if (extFuncs.contains(sym)){
					*res = expr;
					return true;
				}
				
				PrintCurrEnv(GetCurrEnv());
				
				UndefinedError(expr);
				return false;
			}
			
			// function
			case SlangType::List: {
				SlangHeader* head = nullptr;
				SlangList* argIt = (SlangList*)expr;
				NEXT_ARG(&argIt);
				if (!WrappedEvalExpr(((SlangList*)expr)->left,&head)){
					EvalError(expr);
					return false;
				}
				size_t base = frameIndex;
				
				size_t argCount = GetArgCount(argIt);
				
				if (head->type==SlangType::Lambda){
					SlangLambda* lam = (SlangLambda*)head;
					StackAddArg(head);
					size_t headIndex = frameIndex-1;
					base = frameIndex;
					SlangParams* funcParams = lam->params;
					bool variadic = lam->header.flags&FLAG_VARIADIC;
					size_t paramCount = (funcParams) ? funcParams->size : 0;
					if (!variadic&&paramCount!=argCount){
						ArityError(expr,argCount,paramCount);
						return false;
					}
					
					if (variadic){
						while (argIt){
							SlangHeader* fArg = nullptr;
							if (!WrappedEvalExpr(argIt->left,&fArg)){
								EvalError(expr);
								return false;
							}
							StackAddArg(fArg);
							NEXT_ARG(&argIt);
						}
						
						SlangList* list = MakeList(funcArgStack,base,frameIndex);
						lam = (SlangLambda*)funcArgStack[headIndex];
						lam->env->SetSymbol(
							lam->params->params[0],
							(SlangHeader*)list
						);
					} else {
						for (size_t i=0;i<argCount;++i){
							SlangHeader* fArg = nullptr;
							if (!WrappedEvalExpr(argIt->left,&fArg)){
								EvalError(expr);
								return false;
							}
							StackAddArg(fArg);
							NEXT_ARG(&argIt);
						}
						
						lam = (SlangLambda*)funcArgStack[headIndex];
						for (size_t i=0;i<argCount;++i){
							lam->env->SetSymbol(lam->params->params[i],funcArgStack[base+i]);
						}
					}
					
					lam = (SlangLambda*)funcArgStack[headIndex];
					expr = lam->expr;
					envStack[envIndex] = lam->env;
					frameIndex = base;
					--frameIndex;
					continue;
				} else if (head->type==SlangType::Symbol){
					SymbolName symbol = ((SlangObj*)head)->symbol;
					frameIndex = base;
					// handle predefined symbols
					bool success = false;
					switch (symbol){
						case SLANG_DEFINE:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncDefine(argIt,res);
							return success;
						case SLANG_LAMBDA:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncLambda(argIt,res);
							return success;
						case SLANG_LET: {
							if (argCount!=2&&argCount!=3){
								ArityError(expr,argCount,2);
								return false;
							}
							
							SymbolName letName;
							// named let
							if (GetType(argIt->left)==SlangType::Symbol){
								letName = ((SlangObj*)argIt->left)->symbol;
								NEXT_ARG(&argIt);
							} else {
								letName = LET_SELF_SYM;
							}
							
							std::vector<SymbolName> params{};
							std::vector<SlangHeader*> initExprs{};
							params.reserve(4);
							initExprs.reserve(4);
							SlangHeader* paramIt = argIt->left;
							if (paramIt&&GetType(paramIt)!=SlangType::List){
								TypeError(paramIt,GetType(paramIt),SlangType::List);
								return false;
							}
							
							if (!GetLetParams(params,initExprs,(SlangList*)paramIt)){
								EvalError(paramIt);
								return false;
							}
							NEXT_ARG(&argIt);
							SlangHeader* letExpr = argIt->left;
							
							// use this to GC the let env
							SlangLambda* tempLambda = MakeLambda(letExpr,params,false,true);
							SlangEnv* letEnv = tempLambda->env;
							StackAddArg((SlangHeader*)tempLambda);
							
							letEnv->DefSymbol(letName,(SlangHeader*)tempLambda);
							
							size_t i=0;
							envStack[envIndex] = letEnv;
							for (const auto& param : params){
								SlangHeader* initObj;
								if (!WrappedEvalExpr(initExprs[i],&initObj)){
									EvalError(initExprs[i]);
									return false;
								}
								if (!GetCurrEnv()->SetSymbol(param,initObj)){
									//std::cout << "Could not set param '" << param << "' of " << *initObj << '\n';
									EvalError(initExprs[i]);
									return false;
								}
								++i;
							}
							
							tempLambda = (SlangLambda*)funcArgStack[--frameIndex];
							// last call tail call
							expr = tempLambda->expr;
							continue;
						}
						case SLANG_SET:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncSet(argIt,res);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_DO: {
							if (argCount==0){
								ArityError(expr,argCount,1);
								return false;
							}
							// while next arg is not null
							while (argIt->right){
								if (!WrappedEvalExpr(argIt->left,res)){
									EvalError(argIt->left);
									return false;
								}
								NEXT_ARG(&argIt);
							}
							
							// elide last call
							expr = argIt->left;
							continue;
						}
						case SLANG_APPLY: {
							// (apply func args)
							ARITY_EXACT_CHECK(2);
							SlangHeader* head = argIt->left;
							NEXT_ARG(&argIt);
							SlangHeader* args;
							if (!WrappedEvalExpr(argIt->left,&args))
								return false;
							
							if (!IsList(args)){
								EvalError(argIt->left);
								TypeError(args,GetType(args),SlangType::List);
								return false;
							}
							
							SlangList* listHead = AllocateList();
							listHead->left = head;
							listHead->right = args;
							
							expr = (SlangHeader*)listHead;
							continue;
						}
						case SLANG_IF: {
							ARITY_EXACT_CHECK(3);
							bool test;
							SlangHeader* condObj = nullptr;
							if (!WrappedEvalExpr(argIt->left,&condObj)){
								EvalError(expr);
								return false;
							}
							if (!ConvertToBool(condObj,test)){
								TypeError(argIt->left,GetType(argIt->left),SlangType::Bool);
								EvalError(expr);
								return false;
							}
							
							NEXT_ARG(&argIt);
							if (test){
								expr = argIt->left;
								continue;
							}
							NEXT_ARG(&argIt);
							expr = argIt->left;
							continue;
						}
						case SLANG_QUOTE:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncQuote(argIt,res);
							return success;
						case SLANG_NOT:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncNot(argIt,res);
							return success;
						case SLANG_NEG:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncNegate(argIt,res);
							return success;
						case SLANG_LIST:
							success = SlangFuncList(argIt,res);
							return success;
						case SLANG_VEC:
							success = SlangFuncVec(argIt,res);
							return success;
						case SLANG_VEC_ALLOC:
							if (argCount!=1&&argCount!=2){
								ArityError(expr,argCount,1);
								return false;
							}
							success = SlangFuncVecAlloc(argIt,res);
							return success;
						case SLANG_VEC_SIZE:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncVecSize(argIt,res);
							return success;
						case SLANG_VEC_GET:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncVecGet(argIt,res);
							return success;
						case SLANG_VEC_SET:
							ARITY_EXACT_CHECK(3);
							success = SlangFuncVecSet(argIt,res);
							return success;
						case SLANG_PAIR:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncPair(argIt,res);
							return success;
						case SLANG_LEFT:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncLeft(argIt,res);
							return success;
						case SLANG_RIGHT:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncRight(argIt,res);
							return success;
						case SLANG_INC:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncInc(argIt,res);
							return success;
						case SLANG_DEC:	
							ARITY_EXACT_CHECK(1);
							success = SlangFuncDec(argIt,res);
							return success;
						case SLANG_ADD:
							success = SlangFuncAdd(argIt,res);
							return success;
						case SLANG_SUB:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncSub(argIt,res);
							return success;
						case SLANG_MUL:
							success = SlangFuncMul(argIt,res);
							return success;
						case SLANG_DIV:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncDiv(argIt,res);
							return success;
						case SLANG_MOD:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncMod(argIt,res);
							return success;
						case SLANG_SET_LEFT:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncSetLeft(argIt,res);
							return success;
						case SLANG_SET_RIGHT:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncSetRight(argIt,res);
							return success;
						case SLANG_PRINT:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncPrint(argIt,res);
							return success;
						case SLANG_ASSERT:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncAssert(argIt,res);
							return success;
						case SLANG_EQ:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncEq(argIt,res);
							return success;
						case SLANG_IS:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncIs(argIt,res);
							return success;
						case SLANG_IS_NULL:
							ARITY_EXACT_CHECK(1);
							if (!WrappedEvalExpr(argIt->left,res))
								return false;
							
							*res = (SlangHeader*)MakeBool(*res==nullptr);
							return true;
						case SLANG_IS_INT:
							ARITY_EXACT_CHECK(1);
							if (!WrappedEvalExpr(argIt->left,res))
								return false;
							
							*res = (SlangHeader*)MakeBool(GetType(*res)==SlangType::Int);
							return true;
						case SLANG_IS_REAL:
							ARITY_EXACT_CHECK(1);
							if (!WrappedEvalExpr(argIt->left,res))
								return false;
							
							*res = (SlangHeader*)MakeBool(GetType(*res)==SlangType::Real);
							return true;
						case SLANG_IS_NUM: {
							ARITY_EXACT_CHECK(1);
							if (!WrappedEvalExpr(argIt->left,res))
								return false;
							
							SlangType type = GetType(*res);
							*res = (SlangHeader*)MakeBool(type==SlangType::Int||type==SlangType::Real);
							return true;
						}
						case SLANG_IS_STRING:
							ARITY_EXACT_CHECK(1);
							if (!WrappedEvalExpr(argIt->left,res))
								return false;
							
							*res = (SlangHeader*)MakeBool(GetType(*res)==SlangType::String);
							return true;
						case SLANG_IS_PAIR:
							ARITY_EXACT_CHECK(1);
							if (!WrappedEvalExpr(argIt->left,res))
								return false;
							
							*res = (SlangHeader*)MakeBool(GetType(*res)==SlangType::List);
							return true;
						case SLANG_IS_PROC:
							ARITY_EXACT_CHECK(1);
							if (!WrappedEvalExpr(argIt->left,res))
								return false;
							
							*res = (SlangHeader*)MakeBool(GetType(*res)==SlangType::Lambda);
							return true;
						case SLANG_IS_VECTOR:
							ARITY_EXACT_CHECK(1);
							if (!WrappedEvalExpr(argIt->left,res))
								return false;
							
							*res = (SlangHeader*)MakeBool(GetType(*res)==SlangType::Vector);
							return true;
						case SLANG_IS_BOUND: {
							ARITY_EXACT_CHECK(1);
							
							if (!WrappedEvalExpr(argIt->left,res))
								return false;
								
							if (GetType(*res)!=SlangType::Symbol){
								TypeError(*res,GetType(*res),SlangType::Symbol);
								return false;
							}
							
							SymbolName sym = ((SlangObj*)*res)->symbol;
							bool b = true;
							if (!IsPredefinedSymbol(sym)){
								SlangHeader* d;
								b = GetRecursiveSymbol(GetCurrEnv(),sym,&d);
								
								if (!b){
									b = extFuncs.contains(sym);
								}
							}
							*res = (SlangHeader*)MakeBool(b);
							return true;
						}
						default:
							if (!extFuncs.contains(symbol)){
								ProcError(head);
								return false;
							}
							success = extFuncs.at(symbol)(this,argIt,res);
							if (!success)
								EvalError(expr);
							return success;
					}
				} else {
					PrintCurrEnv(GetCurrEnv());
					TypeError(head,GetType(head),SlangType::Lambda);
					return false;
				}
			}
		}
	}
}

void SlangInterpreter::PushError(const SlangHeader* expr,const std::string& msg){
	LocationData loc = parser.GetExprLocation(expr);
	errors.emplace_back(loc,msg);
}

void SlangInterpreter::EvalError(const SlangHeader* expr){
	if (expr==program) return;
	std::stringstream msg = {};
	
	msg << "EvalError:\n    Error while evaluating '";
	if (expr)
		msg << *expr << "'";
	else
		msg << "()'";
	PushError(expr,msg.str());
}

void SlangInterpreter::UndefinedError(const SlangHeader* expr){
	std::stringstream msg = {};
	msg << "UndefinedError:\n    Symbol '";
	if (expr)
		msg << *expr << "' is not defined!";
	else
	 	msg << "()' is not defined!";
		
	
	PushError(expr,msg.str());
}

void SlangInterpreter::RedefinedError(const SlangHeader* expr,SymbolName sym){
	std::stringstream msg = {};
	msg << "RedefinedError:\n    Symbol '" << 
		parser.GetSymbolString(sym) << "' in expr '";
	if (expr)
		msg << *expr << "' was redefined!";
	else
		msg << "()' was redefined!";
	PushError(expr,msg.str());
}

void SlangInterpreter::TypeError(const SlangHeader* expr,SlangType found,SlangType expect,size_t index){
	std::stringstream msg = {};
	msg << "TypeError:\n    Expected type " << 
		TypeToString(expect) << " instead of type " << 
		TypeToString(found) << " in argument '";
	if (expr)
		msg << *expr << "'";
	else
		msg << "()'";
	if (index!=-1ULL){
		msg << " (arg #" << index << ")";
	}
		
	PushError(expr,msg.str());
}

void SlangInterpreter::IndexError(const SlangHeader* expr,size_t size,ssize_t index){
	std::stringstream msg = {};
	msg << "IndexError:\n    Invalid index " << index << " for size " << size;
	PushError(expr,msg.str());
}

void SlangInterpreter::AssertError(const SlangHeader* expr){
	std::stringstream msg = {};
	msg << "AssertError:\n    Assertion failed: '";
	if (expr)
		msg << *expr << "'";
	else
		msg << "()'";
	
	PushError(expr,msg.str());
}

void SlangInterpreter::ProcError(const SlangHeader* proc){
	std::stringstream msg = {};
	msg << "ProcError:\n    Procedure '";
	if (proc)
		msg << *proc << "' is not defined!";
	else
		msg << "()' is not defined!";
	
	PushError(proc,msg.str());
}

void SlangInterpreter::ArityError(const SlangHeader* head,size_t found,size_t expect){
	std::stringstream msg = {};
	const char* plural = (expect!=1) ? "s" : "";
	
	msg << "ArityError:\n    Procedure '";
	if (head)
		msg << *head;
	else
		msg << "()";
		
	msg << "' takes " << expect << " argument" << plural << ", was given " <<
		found;
		
	PushError(head,msg.str());
}

void SlangInterpreter::ZeroDivisionError(const SlangHeader* head){
	std::stringstream msg = {};
	
	msg << "ZeroDivisionError:\n    Division by zero";
		
	PushError(head,msg.str());
}

void SlangInterpreter::Run(SlangHeader* prog){
	arena = new MemArena();
	size_t memSize = SMALL_SET_SIZE*2;
	uint8_t* memAlloc = (uint8_t*)malloc(memSize);
	arena->SetSpace(memAlloc,memSize);
	memset(arena->memSet,0,memSize);
	gDebugInterpreter = this;
	
	funcArgStack.resize(32);
	envStack.resize(32);
	frameIndex = 0;
	envIndex = 0;
	errors = {};
	genIndex = 1;
	
	program = prog;
	SlangHeader* res = nullptr;
	SlangEnv* env = AllocateEnv(4);
	env->parent = nullptr;
	envStack[0] = env;
	
	auto start = std::chrono::steady_clock::now();
	WrappedEvalExpr(prog,&res);
	std::chrono::duration<double> diff = std::chrono::steady_clock::now()-start;
	gRunTime = diff.count();
	
	if (!errors.empty()){
		PrintErrors(errors);
		std::cout << '\n';
		PrintCurrEnv(GetCurrEnv());
	}
	
	frameIndex = 0;
	envIndex = 0;
	SmallGC(0);
	
//	size_t tenureCount = arena->tenureSets.size();
//	arena->ClearTenuredSets();
	
	std::cout << "Alloc balance: " << gAllocCount << '\n';
	std::cout << "Alloc total: " << gAllocTotal << " (" << gAllocTotal/1024 << " KB)\n";
	//std::cout << "Tenure count: " << gTenureCount << " (" << gTenureCount*sizeof(SlangObj)/1024 << " KB)\n";
	//std::cout << "Tenure set count: " << tenureCount << '\n';
	size_t codeSize = parser.codePointer-parser.codeStart;
	std::cout << "Code alloc total: " << codeSize << " (" << codeSize/1024 << " KB)\n";
	std::cout << "Eval count: " << gEvalCounter << '\n';
	std::cout << "Non-eliminated eval count: " << gEvalRecurCounter << '\n';
	std::cout << "Max stack height: " << gMaxStackHeight << '\n';
	std::cout << "Small GCs: " << gSmallGCs << '\n';
	std::cout << "Realloc count: " << gReallocCount << '\n';
	std::cout << "Max arena size: " << gMaxArenaSize/1024 << " KB\n";
	std::cout << "Curr arena size: " << arena->memSize/1024 << " KB\n";
	std::cout << "Curr depth: " << gCurrDepth << '\n';
	std::cout << "Max depth: " << gMaxDepth << '\n';
	std::cout << "Env count: " << gEnvCount << '\n';
	std::cout << "Env balance: " << gEnvBalance << '\n';
	std::cout << std::fixed << std::setprecision(3);
	std::cout << "Parse time: " << gParseTime*1000 << "ms\n";
	if (gRunTime>=2.0)
		std::cout << "Run time: " << gRunTime << "s\n";
	else
		std::cout << "Run Time: " << gRunTime*1000 << "ms\n";
	if (arena->memSet)
		free(arena->memSet);
	delete arena;
}

inline bool IsWhitespace(char c){
	return c==' '||c=='\t'||c=='\n';
}

inline SlangToken SlangTokenizer::NextToken(){
	while (pos!=end&&
			(IsWhitespace(*pos))){
		if (*pos=='\n'){
			++line;
			col = 0;
		} else {
			++col;
		}
		++pos;
	}
	
	if (pos==end) return {SlangTokenType::EndOfFile,{},line,col};
	
	StringIt begin = pos;
	char c = *begin;
	char nextC = ' ';
	if (pos+1!=end)
		nextC = *(pos+1);
	SlangToken token = {};
	token.line = line;
	token.col = col;
	
	if (c=='('||c=='['||c=='{'){
		token.type = SlangTokenType::LeftBracket;
		++pos;
	} else if (c==')'||c==']'||c=='}'){
		token.type = SlangTokenType::RightBracket;
		++pos;
	} else if (c=='\''){
		token.type = SlangTokenType::Quote;
		++pos;
	} else if (c=='!'){
		token.type = SlangTokenType::Not;
		++pos;
	} else if (c=='-'&&nextC!='-'&&!IsWhitespace(nextC)){
		token.type = SlangTokenType::Negation;
		++pos;
	} else if (c=='"'){
		token.type = SlangTokenType::String;
		++pos;
		while (pos!=end&&*pos!='"'){
			if (*pos=='\n'){
				token.type = SlangTokenType::Error;
				break;
			} else if (*pos=='\\'){
				++pos;
			}
			++pos;
		}
		if (pos==end){
			token.type = SlangTokenType::Error;
		} else {
			++pos;
		}
	} else if (c==';'){
		token.type = SlangTokenType::Comment;
		while (pos!=end&&*pos!='\n') ++pos;
	} else if ((c>='0'&&c<='9')||c=='.'){
		token.type = (c=='.') ? SlangTokenType::Real : SlangTokenType::Int;
		++pos;
		while (pos!=end&&*pos>='0'&&*pos<='9'){
			++pos;
		}
		if (pos!=end&&*pos=='.'&&token.type!=SlangTokenType::Real){
			token.type = SlangTokenType::Real;
			++pos;
			while (pos!=end&&*pos>='0'&&*pos<='9'){
				++pos;
			}
		}
		// if unexpected char shows up at end (like '123q')
		if (pos!=end&&*pos!=')'&&*pos!=']'&&*pos!='}'&&!IsWhitespace(*pos)){
			token.type = SlangTokenType::Error;
			++pos;
		}
	} else if (IsIdentifierChar(c)){
		token.type = SlangTokenType::Symbol;
		while (pos!=end&&IsIdentifierChar(c)){
			++pos;
			c = *pos;
		}
	} else {
		token.type = SlangTokenType::Error;
		++pos;
	}
	token.view = {begin,pos};
	if (token.type!=SlangTokenType::String){
		if (token.view=="true"){
			token.type = SlangTokenType::True;
		} else if (token.view=="false"){
			token.type = SlangTokenType::False;
		}
	}
	col += pos-begin;
	return token;
}

SymbolName SlangParser::RegisterSymbol(const std::string& symbol){
	SymbolName name = currentName++;
	
	nameDict[symbol] = name;
	return name;
}

std::string SlangParser::GetSymbolString(SymbolName symbol){
	if (symbol & GEN_FLAG){
		std::stringstream ss{};
		ss << "$tmp" << ((uint64_t)symbol&~GEN_FLAG);
		return ss.str();
	}
		
		
	for (const auto& [key,val] : nameDict){
		if (val==symbol)
			return key;
	}
	
	return "UNDEFINED";
}


void SlangParser::PushError(const std::string& msg){
	LocationData l = {token.line,token.col,0};
	errors.emplace_back(l,msg);
}

inline void SlangParser::NextToken(){
	if (!errors.empty()) return;
	if (token.type==SlangTokenType::EndOfFile){
		PushError("Ran out of tokens!");
		return;
	}
	
	token = tokenizer.NextToken();
	while (token.type==SlangTokenType::Comment){
		token = tokenizer.NextToken();
	}
		
	if (token.type==SlangTokenType::Error){
		PushError("Unexpected character in token: "+std::string(token.view));
	}
}

/*inline SlangObj* HeapAllocObj(){
	++gHeapAllocTotal;
	SlangObj* o = new SlangObj;
	o->flags = 0;
	return o;
}*/

inline void* SlangParser::CodeAlloc(size_t size){
	if (codeEnd-codePointer<(ssize_t)size){
		std::cout << "out of code mem\n";
		exit(1);
	}
	uint8_t* data = codePointer;
	codePointer += size;
	return data;
}

inline SlangObj* SlangParser::CodeAllocObj(){
	SlangObj* obj = (SlangObj*)CodeAlloc(sizeof(SlangObj));
	obj->header.flags = 0;
	return obj;
}

inline SlangStr* SlangParser::CodeAllocStr(size_t size){
	size_t qsize = QuantizeSize(size);
	SlangStorage* storage = (SlangStorage*)CodeAlloc(sizeof(SlangStorage)+sizeof(uint8_t)*qsize);
	SlangStr* obj = (SlangStr*)CodeAlloc(sizeof(SlangStr));
	obj->header.type = SlangType::String;
	obj->header.flags = 0;
	obj->storage = storage;
	storage->size = size;
	storage->capacity = qsize;
	storage->header.elemSize = sizeof(uint8_t);
	return obj;
}

inline SlangList* SlangParser::CodeAllocList(){
	SlangList* list = (SlangList*)CodeAlloc(sizeof(SlangList));
	list->header.type = SlangType::List;
	list->header.flags = 0;
	return list;
}

inline SlangList* SlangParser::WrapExprIn(SymbolName func,SlangHeader* expr){
	SlangObj* q = CodeAllocObj();
	q->header.type = SlangType::Symbol;
	q->symbol = func;

	SlangList* list = CodeAllocList();
	list->left = (SlangHeader*)q;
	SlangList* listright = CodeAllocList();
	listright->left = expr;
	listright->right = nullptr;
	list->right = (SlangHeader*)listright;

	return list;
}

inline SlangList* SlangParser::WrapExprSplice(SymbolName func,SlangList* expr){
	SlangObj* q = CodeAllocObj();
	q->header.type = SlangType::Symbol;
	q->symbol = func;

	SlangList* list = CodeAllocList();
	list->left = (SlangHeader*)q;
	list->right = (SlangHeader*)expr;

	return list;
}

inline std::string GetStringFromToken(std::string_view view){
	std::string s{};
	s.reserve(view.size());
	for (size_t i=0;i<view.size();++i){
		uint8_t c = view[i];
		if (c=='\\'){
			++i;
			if (i>=view.size()) break;
			c = view[i];
			switch (c){
				case 'n':
					s.push_back('\n');
					break;
				case 't':
					s.push_back('\t');
					break;
				case 'v':
					s.push_back('\v');
					break;
				case 'b':
					s.push_back(8);
					break;
				case 'e':
					s.push_back(27);
					break;
				case '0':
					s.push_back(0);
					break;
				case 'f':
					s.push_back('\f');
					break;
				case 'r':
					s.push_back('\r');
					break;
				case '\\':
					s.push_back('\\');
					break;
				case '\'':
					s.push_back('\'');
					break;
				case '"':
					s.push_back('"');
					break;
				default:
					s.push_back('\\');
					s.push_back(c);
					break;
			}
		} else {
			s.push_back(c);
		}
	}
	return s;
}

inline bool SlangParser::ParseObj(SlangHeader** res){
	LocationData loc = {token.line,token.col,0};
	switch (token.type){
		case SlangTokenType::Int: {
			char* d;
			int64_t i = strtoll(token.view.begin(),&d,10);
			NextToken();
			SlangObj* intObj = CodeAllocObj();
			intObj->header.type = SlangType::Int;
			intObj->integer = i;
			codeMap[(SlangHeader*)intObj] = loc;
			*res = (SlangHeader*)intObj;
			return true;
		}
		case SlangTokenType::Real: {
			double r = std::stod(token.view.begin(),nullptr);
			NextToken();
			SlangObj* realObj = CodeAllocObj();
			realObj->header.type = SlangType::Real;
			realObj->real = r;
			codeMap[(SlangHeader*)realObj] = loc;
			*res = (SlangHeader*)realObj;
			return true;
		}
		case SlangTokenType::Symbol: {
			SymbolName s;
			std::string copy = std::string(token.view);
			if (!nameDict.contains(copy))
				s = RegisterSymbol(copy);
			else
				s = nameDict.at(copy);
			NextToken();
			SlangObj* sym = CodeAllocObj();
			sym->header.type = SlangType::Symbol;
			sym->symbol = s;
			codeMap[(SlangHeader*)sym] = loc;
			*res = (SlangHeader*)sym;
			return true;
		}
		case SlangTokenType::String: {
			// get rid of ""
			token.view.remove_prefix(1);
			token.view.remove_suffix(1);
			std::string val = GetStringFromToken(token.view);
			NextToken();
			SlangStr* strObj = CodeAllocStr(val.size());
			strObj->CopyFromString(val);
			codeMap[(SlangHeader*)strObj] = loc;
			*res = (SlangHeader*)strObj;
			return true;
		}
		case SlangTokenType::True: {
			NextToken();
			SlangObj* boolObj = CodeAllocObj();
			boolObj->header.type = SlangType::Bool;
			boolObj->boolean = true;
			codeMap[(SlangHeader*)boolObj] = loc;
			*res = (SlangHeader*)boolObj;
			return true;
		}
		case SlangTokenType::False: {
			NextToken();
			SlangObj* boolObj = CodeAllocObj();
			boolObj->header.type = SlangType::Bool;
			boolObj->boolean = false;
			codeMap[(SlangHeader*)boolObj] = loc;
			*res = (SlangHeader*)boolObj;
			return true;
		}
		case SlangTokenType::Quote: {
			NextToken();
			SlangHeader* sub;
			if (!ParseObj(&sub)) return false;
			*res = (SlangHeader*)WrapExprIn(SLANG_QUOTE,sub);
			codeMap[*res] = loc;
			return true;
		}
		case SlangTokenType::Not: {
			NextToken();
			SlangHeader* sub;
			if (!ParseObj(&sub)) return false;
			*res = (SlangHeader*)WrapExprIn(SLANG_NOT,sub);
			codeMap[*res] = loc;
			return true;
		}
		case SlangTokenType::Negation: {
			NextToken();
			SlangHeader* sub;
			if (!ParseObj(&sub)) return false;
			*res = (SlangHeader*)WrapExprIn(SLANG_NEG,sub);
			codeMap[*res] = loc;
			return true;
		}
		case SlangTokenType::LeftBracket: {
			if (!ParseExpr(res)) return false;
			codeMap[*res] = loc;
			return true;
		}
		case SlangTokenType::Error:
			PushError("Token error: "+std::string(token.view));
			return false;
		case SlangTokenType::EndOfFile:
			// might be superfluous
			PushError("Ran out of tokens!");
			return false;
		default:
			PushError("Unexpected token: "+std::string(token.view));
			return false;
	}
	
	PushError("Error parsing object!");
	return false;
}

// argCount > 1
void SlangParser::TestSeqMacro(SlangList* list,size_t argCount){
	if (GetType(list->left)!=SlangType::Symbol){
		return;
	}
	
	SlangObj* obj = (SlangObj*)list->left;
	switch (obj->symbol){
		case SLANG_LET: {
			if (argCount<=3) return;
			// is named?
			SlangList* next = (SlangList*)list->right;
			if (GetType(next->left)==SlangType::Symbol){
				if (argCount<=4) return;
				SlangList* nextnext = (SlangList*)next->right;
				SlangList* doWrapper = WrapExprSplice(SLANG_DO,(SlangList*)nextnext->right);
				SlangList* newHead = CodeAllocList();
				newHead->left = (SlangHeader*)doWrapper;
				newHead->right = nullptr;
				nextnext->right = (SlangHeader*)newHead;
			} else {
				// (let (()) b1 b2...)
				SlangList* doWrapper = WrapExprSplice(SLANG_DO,(SlangList*)next->right);
				SlangList* newHead = CodeAllocList();
				newHead->left = (SlangHeader*)doWrapper;
				newHead->right = nullptr;
				next->right = (SlangHeader*)newHead;
			}
			return;
		}
		case SLANG_LAMBDA: {
			if (argCount<=3) return;
			SlangList* next = (SlangList*)list->right;
			SlangList* doWrapper = WrapExprSplice(SLANG_DO,(SlangList*)next->right);
			SlangList* newHead = CodeAllocList();
			newHead->left = (SlangHeader*)doWrapper;
			newHead->right = nullptr;
			next->right = (SlangHeader*)newHead;
			return;
		}
		default:
			break;
	}
}

bool SlangParser::ParseExpr(SlangHeader** res){
	*res = nullptr;
	
	SlangList* obj = nullptr;
	size_t argCount = 0;
	char leftBracket = token.view[0];
	NextToken();
	
	while (token.type!=SlangTokenType::RightBracket){
		if (!obj){
			obj = CodeAllocList();
			*res = (SlangHeader*)obj;
		} else {
			obj->right = (SlangHeader*)CodeAllocList();
			obj = (SlangList*)obj->right;
		}
		if (!ParseObj(&obj->left))
			return false;
		
		obj->right = nullptr;
		++argCount;
	}
	
	char rightBracket = token.view[0];
	if ((leftBracket=='('&&rightBracket!=')')||
		(leftBracket=='['&&rightBracket!=']')||
		(leftBracket=='{'&&rightBracket!='}')){
		std::string start = {},end = {};
		start += rightBracket;
		switch (leftBracket){
			case '[':
				end += ']';
				break;
			case '(':
				end += ')';
				break;
			case '{':
				end += '}';
				break;
		}
		PushError("Bracket mismatch! Have '"+start+
				"', expected '"+end+"'.");
		return false;
	}
	
	if (argCount>1){
		TestSeqMacro((SlangList*)*res,argCount);
	}
	
	NextToken();
	return true;
}

// wraps program exprs in a (do ...) block
inline SlangHeader* SlangParser::WrapProgram(const std::vector<SlangHeader*>& exprs){
	SlangObj* doSym = CodeAllocObj();
	doSym->header.type = SlangType::Symbol;
	doSym->symbol = SLANG_DO;

	SlangList* outer = CodeAllocList();
	outer->left = (SlangHeader*)doSym;
	outer->right = nullptr;
	
	if (exprs.empty()) return (SlangHeader*)outer;
	
	SlangList* next = nullptr;
	for (auto* expr : exprs){
		if (!next){
			next = CodeAllocList();
			outer->right = (SlangHeader*)next;
		} else {
			next->right = (SlangHeader*)CodeAllocList();
			next = (SlangList*)next->right;
		}
		next->left = expr;
		next->right = nullptr;
	}
	
	return (SlangHeader*)outer;
}
/*
SlangObj* SlangParser::LoadIntoBuffer(SlangObj* prog){
	size_t i = codeIndex++;
	codeBuffer[i] = *prog;
	
	if (prog->type==SlangType::List){
		prog->left = LoadIntoBuffer(prog->left);
		prog->right = LoadIntoBuffer(prog->right);
	}
	
	return &codeBuffer[i];
}*/

SlangHeader* SlangParser::Parse(const std::string& code){
	gDebugParser = this;
	
//	codeBuffer = {};
//	codeIndex = 0;
//	size_t codeStart = gHeapAllocTotal;
	
	auto start = std::chrono::steady_clock::now();
	
	tokenizer.SetText(code);
	token.type = SlangTokenType::Comment;
	NextToken();
	
	std::vector<SlangHeader*> exprs = {};
	SlangHeader* temp;
	while (errors.empty()){
		if (!ParseObj(&temp)) break;
		exprs.push_back(temp);
		
		if (token.type==SlangTokenType::EndOfFile) break;
	}
	
	if (!errors.empty()){
		PrintErrors(errors);
		return nullptr;
	}
	
	SlangHeader* prog = WrapProgram(exprs);
	
	std::chrono::duration<double> diff = std::chrono::steady_clock::now()-start;
	gParseTime = diff.count();
	
//	codeCount = gHeapAllocTotal - codeStart;
//	codeBuffer.resize(codeCount);
	//LoadIntoBuffer(prog);
	
	return prog;
}

inline LocationData SlangParser::GetExprLocation(const SlangHeader* expr){
	if (!codeMap.contains(expr))
		return {-1U,-1U,-1U};
	
	return codeMap.at(expr);
}

SlangParser::SlangParser() :
		tokenizer(),token(),
		nameDict(gDefaultNameDict),
		currentName(GLOBAL_SYMBOL_COUNT),errors(){
	token.type = SlangTokenType::Comment;
	
	codeSize = 32768;
	codeStart = (uint8_t*)malloc(codeSize);
	codePointer = codeStart;
	codeEnd = codeStart + codeSize;
}

SlangParser::~SlangParser(){
	free(codeStart);
}

inline void SlangInterpreter::AddExternalFunction(const ExternalFunc& ext){
	SymbolName sym = parser.RegisterSymbol(ext.name);
	extFuncs[sym] = ext.func;
}

SlangInterpreter::SlangInterpreter(){
	extFuncs = {};
	AddExternalFunction({"gc-collect",&GCManualCollect});
	AddExternalFunction({"gc-mem-size",GCMemSize});
	AddExternalFunction({"gc-mem-cap",&GCMemCapacity});
}

SlangHeader* SlangInterpreter::Parse(const std::string& code){
	return parser.Parse(code);
}

std::ostream& operator<<(std::ostream& os,const SlangHeader& obj){
	switch (obj.type){
		case SlangType::Env:
			os << "[ENV]";
			break;
		case SlangType::Params:
			os << "[PARAMS]";
			break;
		case SlangType::Storage:
			os << "[STORAGE]";
			break;
		case SlangType::NullType:
			os << "[NULL]";
			break;
		case SlangType::Int:
			os << ((SlangObj*)&obj)->integer;
			break;
		case SlangType::Real:
			os << ((SlangObj*)&obj)->real;
			break;
		case SlangType::Lambda: {
			bool variadic = obj.flags&FLAG_VARIADIC;
			SlangLambda* lam = (SlangLambda*)&obj;
			os << "(& ";
			if (!variadic)
				os << "(";
			
			if (lam->params){
				for (size_t i=0;i<lam->params->size;++i){
					if (i!=0)
						os << ' ';
					os << gDebugParser->GetSymbolString(lam->params->params[i]);
				}
			}
			if (!variadic)
				os << ")";
			os << " ";
			if (lam->expr)
				os << *lam->expr << ")";
			else
				os << "())";
			break;
		}
		case SlangType::Symbol:
			os << gDebugParser->GetSymbolString(((SlangObj*)&obj)->symbol);
			break;
		case SlangType::String: {
			SlangStorage* storage = ((SlangStr*)&obj)->storage;
			os << '"';
			uint8_t c;
			for (size_t i=0;i<storage->size;++i){
				c = storage->data[i];
				if (!IsPrintable(c)){
					switch (c){
						case '\n':
							os << "\\n";
							break;
						case '\r':
							os << "\\r";
							break;
						case '\t':
							os << "\\t";
							break;
						case '\v':
							os << "\\v";
							break;
						case '\a':
							os << "\\a";
							break;
						case '\f':
							os << "\\f";
							break;
						case '\b':
							os << "\\b";
							break;
						case 27:
							os << "\\e";
							break;
						default:
							os << "\\x" << std::hex << (c>>4) << (c&0xf) << std::dec;
							break;
					}
				} else {
					os << c;
				}
			}
			os << '"';
			break;
		}
		case SlangType::Vector: {
			SlangStorage* storage = ((SlangVec*)&obj)->storage;
			
			os << "#[";
			for (size_t i=0;i<storage->size;++i){
				if (i!=0)
					os << ' ';
				if (!storage->objs[i])
					os << "()";
				else
					os << *storage->objs[i];
			}
			os << "]";
			break;
		}
		case SlangType::Bool:
			if (((SlangObj*)&obj)->boolean){
				os << "true";
			} else {
				os << "false";
			}
			break;
		case SlangType::List: {
			SlangList* list = (SlangList*)&obj;
			os << '(';
			if (list->left)
				os << *list->left;
			else
				os << "()";
			SlangList* next = (SlangList*)list->right;
			while (next){
				os << ' ';
				if(next->header.type!=SlangType::List){
					os << ". " << *(SlangHeader*)next;
					break;
				}
				if (next->left){
					os << *next->left;
				} else {
					os << "()";
				}
				next = (SlangList*)next->right;
			}
			os << ')';
			break;
		}
	}
	
	return os;
}

}
