#include "slang.h"

#include <assert.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <math.h>
#include <time.h>
#include <chrono>
#include <set>
#include <filesystem>

#define GEN_FLAG (1ULL<<63)
#define LET_SELF_SYM GEN_FLAG
#define EMPTY_NAME (-1ULL)
#define SLANG_FILE_EXT ".sl"

namespace slang {

#ifndef NDEBUG
size_t gAllocTotal = 0;
#endif
size_t gNameCollisions = 0;
size_t gHeapAllocTotal = 0;
size_t gMaxStackHeight = 0;
size_t gSmallGCs = 0;
size_t gReallocCount = 0;
size_t gMaxArenaSize = 0;
size_t gArenaSize = 0;
size_t gCurrDepth = 0;
size_t gMaxDepth = 0;
size_t gCaseMisses = 0;
double gParseTime = 0.0;
double gCompileTime = 0.0;
double gRunTime = 0.0;

size_t gEvalCounter = 0;
size_t gEvalRecurCounter = 0;

size_t GetNumberSize(int64_t n){
	size_t c = 1;
	if (n<0){
		++c;
		n = -n;
	}
	
	while (n>9){
		n /= 10;
		++c;
	}
	return c;
}

SlangParser* gDebugParser;
CodeInterpreter* gDebugInterpreter;

inline bool IsIdentifierChar(char c){
	return (c>='a'&&c<='z') ||
		   (c>='A'&&c<='Z') ||
		   (c>='0'&&c<='9') ||
		   (c>='$'&&c<='\'') ||
		   (c>='*'&&c<='/') ||
		   (c>='<'&&c<='@') ||
		   c=='_'||c=='!'||c=='^'||
		   c=='~'||c=='|';
}

inline bool CheckFileExists(const std::string& path){
	std::error_code ec;
	return std::filesystem::exists(path,ec);
}

inline bool TryRemoveFile(const std::string& path){
	std::error_code ec;
	return std::filesystem::remove(path,ec);
}

#define DEF_SYM(name,_2,_3,_4,_5) name,
enum SlangGlobalSymbol {
	#include "symbols.cpp.inc"
	// tail recursive
	GLOBAL_SYMBOL_COUNT,
	// not a true symbol
	SLANG_ELSE = GLOBAL_SYMBOL_COUNT,
};
#undef DEF_SYM

struct ArityInfo {
	uint16_t min;
	uint16_t max;
};

#define DEF_SYM(_1,_2,minArity,maxArity,_5) {minArity, maxArity},
static const ArityInfo gGlobalArityArray[] = {
	#include "symbols.cpp.inc"
};
#undef DEF_SYM

enum SlangPurity {
	SLANG_PURE,
	SLANG_HEAD_PURE,
	SLANG_IMPURE
};

#define DEF_SYM(_1,_2,_3,_4,pure) pure,
static const uint8_t gGlobalPurityArray[] = {
	#include "symbols.cpp.inc"
};
#undef DEF_SYM

bool GoodArity(SymbolName sym,size_t argCount){
	ArityInfo info = gGlobalArityArray[sym];
	if (argCount<(size_t)info.min||argCount>(size_t)info.max){
		return false;
	}
	return true;
}

#define DEF_SYM(_1,strName,_3,_4,_5) strName,
static const std::vector<const char*> gDefaultNameArray = {
	#include "symbols.cpp.inc"
	"else",
};
#undef DEF_SYM

bool PrintLineFromSourceFile(const LocationData& loc){
	if (loc.moduleName==-1U)
		return false;
	std::string filename = gDebugInterpreter->GetModuleString(loc.moduleName);
	
	std::ifstream f{filename};
	if (!f)
		return false;
		
	size_t currLine = 0;
	std::string line;
	while (currLine<loc.line){
		std::getline(f,line);
		if (!f) return false;
		++currLine;
	}
	std::getline(f,line);
	
	if (currLine<10)
		std::cout << ' ';
	if (currLine<100)
		std::cout << ' ';
		
	std::cout << loc.line+1 << " | ";
	std::cout << line << '\n';
	size_t numW = GetNumberSize(loc.line+1);
	if (numW<3) numW = 3;
	size_t colIndex = loc.col+numW+3;
	for (size_t i=0;i<colIndex;++i){
		ssize_t real = i-numW-3;
		if (real>=0&&line[real]=='\t')
			std::cout << '\t';
		else
			std::cout << ' ';
	}
	std::cout << "^\n";
	
	return true;
}

void PrintErrors(const std::vector<ErrorData>& errors){
	for (auto it=errors.rbegin();it!=errors.rend();++it){
		auto error = *it;
		if (error.loc.moduleName==-1U)
			std::cout << "<eval>";
		else
			std::cout << gDebugInterpreter->GetModuleString(error.loc.moduleName);
		std::cout << ":" << 
			error.loc.line+1 << ',' << error.loc.col+1 << '\n';
		std::cout << error.type << ":\n";
		if (!error.message.empty())
			std::cout << "    " << error.message << "\n";
		
		if (error.loc.moduleName!=-1U)
			PrintLineFromSourceFile(error.loc);
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
		std::string_view name = gDebugParser->GetSymbolString(mapping.sym);
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

struct SFCState {
	uint64_t a;
	uint64_t b;
	uint64_t c;
	uint64_t counter;
	
	inline uint64_t Get64(){
		uint64_t tmp = a+b+counter++;
		a = b^(b>>11);
		b = c+(c<<3);
		c = ((c<<24)|(c>>(64-24)))+tmp;
		return tmp;
	}
	
	inline void Seed(uint64_t s){
		a = s;
		b = s;
		c = s;
		counter = 1;
		for (size_t i=0;i<12;++i) Get64();
	}
};

SFCState gRandState;

inline double GetDoubleTime(){
	struct timespec ts;
	timespec_get(&ts,TIME_UTC);
	return (double)ts.tv_sec + ((double)ts.tv_nsec)*1e-9;
}

inline double GetDoublePerfTime(){
	struct timespec ts;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&ts);
	return (double)ts.tv_sec + ((double)ts.tv_nsec)*1e-9;
}

inline uint64_t GetSeedTime(){
	struct timespec ts;
	timespec_get(&ts,TIME_UTC);
	uint64_t t = ts.tv_sec*1000 + ts.tv_nsec/1000000;
	return t;
}

inline const char* TypeToString(SlangType type){
	switch (type){
		case SlangType::Int:
			return "Int";
		case SlangType::Real:
			return "Real";
		case SlangType::Maybe:
			return "Maybe";
		case SlangType::Vector:
			return "Vec";
		case SlangType::String:
			return "Str";
		case SlangType::Dict:
			return "Dict";
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
		case SlangType::DictTable:
			return "DictTable";
		case SlangType::InputStream:
			return "IStream";
		case SlangType::OutputStream:
			return "OStream";
		case SlangType::EndOfFile:
			return "EndOfFile";
		case SlangType::NullType:
			return "Null";
	}
	
	static char errArr[32];
	sprintf(errArr,"Invalid %d",(int)type);
	return errArr;
}

inline size_t SlangHeader::GetSize() const {
	switch (type){
		case SlangType::Storage: {
			SlangStorage* storage = (SlangStorage*)this;
			return sizeof(SlangStorage)+storage->capacity*elemSize;
		}
		case SlangType::DictTable: {
			SlangDictTable* storage = (SlangDictTable*)this;
			return sizeof(SlangDictTable)+storage->capacity*sizeof(size_t);
		}
		case SlangType::Vector: {
			return sizeof(SlangVec);
		}
		case SlangType::String: {
			return sizeof(SlangStr);
		}
		case SlangType::Dict: {
			return sizeof(SlangDict);
		}
		case SlangType::Env: {
			return sizeof(SlangEnv);
		}
		case SlangType::Params: {
			SlangParams* params = (SlangParams*)this;
			return sizeof(SlangParams)+params->size*sizeof(SymbolName);
		}
		case SlangType::InputStream:
		case SlangType::OutputStream:
			return sizeof(SlangStream);
		case SlangType::Int:
		case SlangType::Real:
		case SlangType::Symbol:
		case SlangType::Maybe:
			return sizeof(SlangObj);
		case SlangType::Lambda:
			return sizeof(SlangLambda);
		case SlangType::List:
			return sizeof(SlangList);
		case SlangType::EndOfFile:
		case SlangType::Bool:
			return sizeof(SlangHeader);
		case SlangType::NullType:
			assert(false);
			return 0;
	}
	assert(false);
	return 0;
}

bool IsHashable(const SlangHeader* obj){
	if (!obj) return true;
	switch (obj->type){
		case SlangType::Bool:
		case SlangType::Int:
		case SlangType::Real:
		case SlangType::Symbol:
		case SlangType::EndOfFile:
		case SlangType::Lambda:
		case SlangType::Vector:
		case SlangType::String:
		case SlangType::List:
		case SlangType::Maybe:
			return true;
	
		case SlangType::NullType:
		case SlangType::Dict:
		case SlangType::Params:
		case SlangType::Env:
		case SlangType::Storage:
		case SlangType::DictTable:
		case SlangType::InputStream:
		case SlangType::OutputStream:
			return false;
	}
	return false;
}

uint64_t SlangHashObj(const SlangHeader* obj){
	if (!obj) return 3;
	switch (obj->type){
		case SlangType::EndOfFile:
			return 4;
		case SlangType::Bool:
			return obj->boolVal;
		case SlangType::Int:
			return ((SlangObj*)obj)->integer;
		case SlangType::Real:
			return ((SlangObj*)obj)->integer;
		case SlangType::Symbol:
			return ((SlangObj*)obj)->symbol;
		case SlangType::Lambda:
			return ((SlangLambda*)obj)->funcIndex;
		case SlangType::Vector: {
			SlangVec* vec = (SlangVec*)obj;
			if (!vec->storage) return 5;
			uint64_t h = 5;
			for (size_t i=0;i<vec->storage->size;++i){
				h = rotleft(h,1);
				h ^= SlangHashObj(vec->storage->objs[i]);
			}
			return h^vec->storage->size;
		}
		case SlangType::String: {
			SlangStr* str = (SlangStr*)obj;
			if (!str->storage) return 6;
			uint64_t h = 6;
			for (size_t i=0;i<str->storage->size;++i){
				h = rotleft(h,1);
				h ^= str->storage->data[i];
			}
			return h^str->storage->size;
		}
		case SlangType::List: {
			SlangList* list = (SlangList*)obj;
			uint64_t h = 7^SlangHashObj(list->left);
			h ^= rotleft(SlangHashObj(list->right),3);
			return h;
		}
		case SlangType::Maybe: {
			SlangObj* o = (SlangObj*)obj;
			uint64_t h = 15;
			if (obj->flags & FLAG_MAYBE_OCCUPIED)
				h ^= SlangHashObj(o->maybe);
			return h;
		}
		
		case SlangType::NullType:
		case SlangType::Params:
		case SlangType::Dict:
		case SlangType::Env:
		case SlangType::Storage:
		case SlangType::DictTable:
		case SlangType::InputStream:
		case SlangType::OutputStream:
			return 0;
	}
	return 0;
}

struct EvacData {
	uint8_t** write;
	MemArena* arena;
};

typedef void(*WalkFunc)(SlangHeader** ref,void* data);

inline void SlangWalkRefs(SlangHeader* obj,WalkFunc func,void* data){
	switch (obj->type){
		case SlangType::List: {
			SlangList* list = (SlangList*)obj;
			func(&list->left,data);
			func(&list->right,data);
			return;
		}
		case SlangType::Lambda: {
			SlangLambda* lam = (SlangLambda*)obj;
			func((SlangHeader**)&lam->env,data);
			return;
		}
		case SlangType::Vector: {
			SlangVec* vec = (SlangVec*)obj;
			func((SlangHeader**)&vec->storage,data);
			
			SlangStorage* storage = vec->storage;
			if (storage){
				for (size_t i=0;i<storage->size;++i){
					func(&storage->objs[i],data);
				}
			}
			return;
		}
		case SlangType::Dict: {
			SlangDict* dict = (SlangDict*)obj;
			func((SlangHeader**)&dict->table,data);
			func((SlangHeader**)&dict->storage,data);
			
			SlangStorage* storage = dict->storage;
			if (storage){
				for (size_t i=0;i<storage->size;++i){
					SlangDictElement* elem = &storage->elements[i];
					if ((uint64_t)elem->key != DICT_UNOCCUPIED_VAL){
						func(&elem->key,data);
						func(&elem->val,data);
					}
				}
			}
			return;
		}
		case SlangType::Env: {
			SlangEnv* env = (SlangEnv*)obj;
			for (size_t i=0;i<env->header.varCount;++i){
				func(&env->mappings[i].obj,data);
			}
			func((SlangHeader**)&env->next,data);
			func((SlangHeader**)&env->parent,data);
			return;
		}
		case SlangType::String: {
			SlangStr* str = (SlangStr*)obj;
			func((SlangHeader**)&str->storage,data);
			return;
		}
		case SlangType::Maybe: {
			SlangObj* maybe = (SlangObj*)obj;
			if (maybe->header.flags & FLAG_MAYBE_OCCUPIED){
				func(&maybe->maybe,data);
			}
			return;
		}
		case SlangType::InputStream:
		case SlangType::OutputStream: {
			SlangStream* stream = (SlangStream*)obj;
			if (!stream->header.isFile){
				func((SlangHeader**)&stream->str,data);
				if (stream->str)
					func((SlangHeader**)&stream->str->storage,data);
			}
			return;
		}
		
		case SlangType::Params:
		case SlangType::Int:
		case SlangType::Real:
		case SlangType::Bool:
		case SlangType::Symbol:
		case SlangType::Storage:
		case SlangType::DictTable:
		case SlangType::EndOfFile:
		case SlangType::NullType:
			return;
	}
	assert(false);
}

inline SlangHeader* Evacuate(SlangHeader* obj,EvacData* data){
	if (!obj) return nullptr;
	if (!data->arena->InCurrSet((uint8_t*)obj)){
		
		return obj;
	}
	
	SlangHeader* toPtr;
	
	toPtr = (SlangHeader*)*data->write;
	size_t s = obj->GetSize();
	*data->write += s;
	memcpy(toPtr,obj,s);
	
	obj->Forward(toPtr);
	
	return obj->GetForwardAddress();
}

void EvacuateOrForward(SlangHeader** obj,EvacData* data){
	if (*obj && (*obj)->IsForwarded())
		*obj = (*obj)->GetForwardAddress();
	else
		*obj = Evacuate(*obj,data);
}

inline void Scavenge(uint8_t* read,EvacData* data){
	while (read<*data->write){
		SlangHeader* obj = (SlangHeader*)read;
		read += obj->GetSize();
		SlangWalkRefs(obj,(WalkFunc)&EvacuateOrForward,(void*)data);
	}
}

inline void ForwardObjWalker(SlangHeader** obj,void*){
	if (*obj && (*obj)->IsForwarded())
		*obj = (*obj)->GetForwardAddress();
}

inline void CodeInterpreter::ReallocSet(size_t newSize){
	newSize = QuantizeSize16(newSize);
	// shrinking and the arena sets have good parity
	if (newSize<=arena->memSize&&arena->currSet<arena->otherSet){
		// no pointer changes needed
	#ifdef NDEBUG
		uint8_t* newPtr = (uint8_t*)realloc(arena->memSet,newSize);
	#else
		uint8_t* oldPtr = arena->memSet;
		uint8_t* newPtr = (uint8_t*)realloc(arena->memSet,newSize);
		assert(newPtr==oldPtr);
	#endif
		arena->SetSpace(newPtr,newSize,arena->currPointer);
		return;
	}
	
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
		obj->Forward((SlangHeader*)newStart);
		start += s;
		newStart += s;
	}
	
	newStart = newPtr;
	while (newStart<newCurrPointer){
		SlangHeader* obj = (SlangHeader*)newStart;
		newStart += obj->GetSize();
		SlangWalkRefs(obj,&ForwardObjWalker,nullptr);
	}
	
	for (size_t i=0;i<modules.size;++i){
		if (modules.data[i].exportEnv && modules.data[i].exportEnv->header.IsForwarded())
			modules.data[i].exportEnv = 
				(SlangEnv*)modules.data[i].exportEnv->header.GetForwardAddress();
		if (modules.data[i].globalEnv && modules.data[i].globalEnv->header.IsForwarded())
			modules.data[i].globalEnv = 
				(SlangEnv*)modules.data[i].globalEnv->header.GetForwardAddress();
	}
	
	for (size_t i=0;i<argStack.size;++i){
		if (argStack.data[i] && argStack.data[i]->IsForwarded())
			argStack.data[i] = argStack.data[i]->GetForwardAddress();
	}
	for (size_t i=0;i<funcStack.size;++i){
		if (funcStack.data[i].env && funcStack.data[i].env->header.IsForwarded())
			funcStack.data[i].env = (SlangEnv*)funcStack.data[i].env->header.GetForwardAddress();
		if (funcStack.data[i].globalEnv && funcStack.data[i].globalEnv->header.IsForwarded())
			funcStack.data[i].globalEnv = 
				(SlangEnv*)funcStack.data[i].globalEnv->header.GetForwardAddress();
	}
	
	if (lamEnv && lamEnv->header.IsForwarded())
		lamEnv = (SlangEnv*)lamEnv->header.GetForwardAddress();
		
	for (size_t i=0;i<finalizers.size;++i){
		auto& finalizer = finalizers.data[i];
		if (finalizer.obj->IsForwarded())
			finalizer.obj = finalizer.obj->GetForwardAddress();
	}
	
	free(arena->memSet);
	arena->SetSpace(newPtr,newSize,newCurrPointer);
	gMaxArenaSize = (newSize > gMaxArenaSize) ?
						newSize : gMaxArenaSize;
	gArenaSize = newSize;
	++gReallocCount;
}

inline void CodeInterpreter::SmallGC(size_t allocAttempt){
	uint8_t* write = arena->otherSet;
	uint8_t* read = write;
	EvacData data = {&write,arena};
	
	for (size_t i=0;i<argStack.size;++i){
		EvacuateOrForward(&argStack.data[i],&data);
	}
	
	for (size_t i=0;i<funcStack.size;++i){
		EvacuateOrForward((SlangHeader**)&funcStack.data[i].env,&data);
		EvacuateOrForward((SlangHeader**)&funcStack.data[i].globalEnv,&data);
	}
	
	for (size_t i=0;i<modules.size;++i){
		EvacuateOrForward((SlangHeader**)&modules.data[i].exportEnv,&data);
		EvacuateOrForward((SlangHeader**)&modules.data[i].globalEnv,&data);
	}
	
	EvacuateOrForward((SlangHeader**)&lamEnv,&data);
	
	Scavenge(read,&data);
	
	arena->SwapSets();
	arena->currPointer = write;
	
	for (size_t i=0;i<finalizers.size;++i){
		auto& finalizer = finalizers.data[i];
		if (!finalizer.obj->IsForwarded()){
			finalizer.func(this,finalizer.obj);
			finalizers.data[i] = finalizers.data[--finalizers.size];
		} else {
			finalizer.obj = finalizer.obj->GetForwardAddress();
		}
	}
	
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

inline void* CodeInterpreterAllocate(void* data,size_t mem){
	CodeInterpreter* c = (CodeInterpreter*)data;
	assert((mem&7)==0);
	//c->SmallGC(mem);
	if (c->arena->currPointer+mem<c->arena->currSet+c->arena->memSize/2){
#ifndef NDEBUG
		gAllocTotal += mem;
#endif
		void* d = c->arena->currPointer;
		c->arena->currPointer += mem;
		return d;
	}
	
	c->SmallGC(mem);
	if (c->arena->currPointer+mem>=c->arena->currSet+c->arena->memSize/2){
		std::cout << "tried to allocate " << mem << " bytes\n";
		std::cout << "already have " << c->arena->currPointer-c->arena->currSet << " in curr set\n";
		std::cout << "out of mem\n";
		exit(1);
		return nullptr;
	}
	
#ifndef NDEBUG
	gAllocTotal += mem;
#endif
	void* d = c->arena->currPointer;
	c->arena->currPointer += mem;
	return d;
}

inline uint8_t* SlangAllocator::Allocate(size_t size){
	return (uint8_t*)alloc(user,size);
}

inline SlangObj* SlangAllocator::AllocateObj(SlangType type){
	SlangObj* p = (SlangObj*)alloc(user,sizeof(SlangHeader)+sizeof(uint64_t));
	p->header.type = type;
	p->header.flags = 0;
	return p;
}

inline SlangLambda* SlangAllocator::AllocateLambda(){
	SlangLambda* lam = (SlangLambda*)alloc(user,sizeof(SlangLambda));
	lam->header.type = SlangType::Lambda;
	lam->header.flags = 0;
	lam->env = nullptr;
	return lam;
}

inline SlangList* SlangAllocator::AllocateList(){
	SlangList* p = (SlangList*)alloc(user,sizeof(SlangList));
	p->header.type = SlangType::List;
	p->header.flags = 0;
	return p;
}

inline SlangParams* SlangAllocator::AllocateParams(size_t size){
	SlangParams* obj = (SlangParams*)alloc(user,sizeof(SlangParams)+sizeof(SymbolName)*size);
	obj->header.type = SlangType::Params;
	obj->header.flags = 0;
	obj->size = size;
	return obj;
}

inline SlangStorage* SlangAllocator::AllocateStorage(size_t size,uint16_t elemSize){
	size_t byteCount = QuantizeSize(size*elemSize);
	SlangStorage* obj = (SlangStorage*)alloc(user,sizeof(SlangStorage)+byteCount);
	obj->header.type = SlangType::Storage;
	obj->header.flags = 0;
	obj->header.elemSize = elemSize;
	obj->size = size;
	obj->capacity = byteCount/elemSize;
	return obj;
}

inline SlangVec* SlangAllocator::AllocateVec(size_t size){
	if (!size){
		SlangVec* obj = (SlangVec*)alloc(user,sizeof(SlangVec));
		obj->header.type = SlangType::Vector;
		obj->header.flags = 0;
		obj->storage = nullptr;
		return obj;
	}
	
	uint8_t* data = (uint8_t*)alloc(user,sizeof(SlangVec)+sizeof(SlangStorage)+size*sizeof(SlangHeader*));
	SlangVec* v = (SlangVec*)data;
	SlangStorage* s = (SlangStorage*)(data+sizeof(SlangVec));
	v->header.type = SlangType::Vector;
	v->header.flags = 0;
	v->storage = s;
	s->header.type = SlangType::Storage;
	s->header.flags = 0;
	s->header.elemSize = sizeof(SlangHeader*);
	s->capacity = size;
	s->size = size;
	return v;
}

inline SlangStr* SlangAllocator::AllocateStr(size_t size){
	if (!size){
		SlangStr* obj = (SlangStr*)alloc(user,sizeof(SlangStr));
		obj->header.type = SlangType::String;
		obj->header.flags = 0;
		obj->storage = nullptr;
		return obj;
	}
	
	size_t qSize = QuantizeSize(size);
	
	uint8_t* data = (uint8_t*)alloc(user,sizeof(SlangStr)+sizeof(SlangStorage)+qSize);
	SlangStr* str = (SlangStr*)data;
	SlangStorage* s = (SlangStorage*)(data+sizeof(SlangStr));
	str->header.type = SlangType::String;
	str->header.flags = 0;
	str->storage = s;
	s->header.type = SlangType::Storage;
	s->header.flags = 0;
	s->header.elemSize = 1;
	s->capacity = qSize;
	s->size = size;
	return str;

}

inline SlangStorage* SlangAllocator::AllocateDictStorage(size_t size){
	size_t byteCount = QuantizeSize(size*sizeof(SlangDictElement));
	SlangStorage* obj = (SlangStorage*)alloc(user,sizeof(SlangStorage)+byteCount);
	obj->header.type = SlangType::Storage;
	obj->header.flags = 0;
	obj->header.elemSize = sizeof(SlangDictElement);
	obj->size = 0;
	obj->capacity = size;
	return obj;
}

inline SlangDictTable* SlangAllocator::AllocateDictTable(size_t size){
	size_t byteCount = size*sizeof(size_t);
	SlangDictTable* obj = (SlangDictTable*)alloc(user,sizeof(SlangDictTable)+byteCount);
	obj->header.type = SlangType::DictTable;
	obj->header.flags = 0;
	obj->capacity = size;
	obj->size = 0;
	return obj;
}

inline SlangDict* SlangAllocator::AllocateDict(){
	SlangDict* obj = (SlangDict*)alloc(user,sizeof(SlangDict));
	obj->header.type = SlangType::Dict;
	obj->header.flags = 0;
	obj->storage = nullptr;
	obj->table = nullptr;
	return obj;
}

inline SlangEnv* SlangAllocator::AllocateEnv(){
	SlangEnv* obj = (SlangEnv*)alloc(user,sizeof(SlangEnv));
	obj->header.type = SlangType::Env;
	obj->header.flags = 0;
	obj->header.varCount = 0;
	obj->parent = nullptr;
	obj->next = nullptr;
	return obj;
}

inline SlangStream* SlangAllocator::AllocateStream(SlangType type){
	SlangStream* stream = (SlangStream*)alloc(user,sizeof(SlangStream));
	stream->header.type = type;
	stream->header.flags = 0;
	stream->str = nullptr;
	return stream;
}

inline SlangObj* SlangAllocator::MakeInt(int64_t i){
	SlangObj* obj = AllocateObj(SlangType::Int);
	obj->integer = i;
	return obj;
}

inline SlangObj* SlangAllocator::MakeReal(double r){
	SlangObj* obj = AllocateObj(SlangType::Real);
	obj->real = r;
	return obj;
}

inline SlangHeader* SlangAllocator::MakeBool(bool b){
	SlangHeader* obj = (SlangHeader*)alloc(user,sizeof(SlangHeader));
	obj->type = SlangType::Bool;
	obj->flags = 0;
	obj->boolVal = b;
	return obj;
}

inline SlangObj* SlangAllocator::MakeSymbol(SymbolName symbol){
	SlangObj* obj = AllocateObj(SlangType::Symbol);
	obj->symbol = symbol;
	return obj;
}

inline SlangHeader* SlangAllocator::MakeEOF(){
	SlangHeader* obj = (SlangHeader*)alloc(user,sizeof(SlangHeader));
	obj->type = SlangType::EndOfFile;
	obj->flags = 0;
	return obj;
}

// delete this
inline SlangParams* SlangAllocator::MakeParams(const std::vector<SymbolName>& args){
	SlangParams* params = AllocateParams(args.size());
	size_t i=0;
	for (const auto& arg : args){
		params->params[i++] = arg;
	}
	return params;
}

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

inline bool SlangEnv::GetSymbol(SymbolName name,SlangHeader** res) const {
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

inline bool SlangEnv::HasSymbol(SymbolName name) const {
	for (size_t i=0;i<header.varCount;++i){
		if (mappings[i].sym==name){
			return true;
		}
	}
	if (next){
		return next->HasSymbol(name);
	}
	
	return false;
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

size_t GetRecursiveSize(SlangHeader* obj){
	if (!obj) return 0;
	
	switch (obj->type){
		case SlangType::Int:
		case SlangType::Real:
		case SlangType::Bool:
		case SlangType::Symbol:
		case SlangType::Params:
		case SlangType::Storage:
		case SlangType::DictTable:
		case SlangType::EndOfFile:
			return obj->GetSize();
		
		case SlangType::List: {
			SlangList* l = (SlangList*)obj;
			size_t c=0;
			while (true){
				if (GetType(l->right)!=SlangType::List)
					return c+GetRecursiveSize(l->left)+GetRecursiveSize(l->right)+l->header.GetSize();
			
				c += GetRecursiveSize(l->left);
				c += l->header.GetSize();
				l = (SlangList*)l->right;
			}
			return c;
		}
		
		case SlangType::Vector: {
			size_t c = obj->GetSize();
			SlangVec* vec = (SlangVec*)obj;
			if (vec->storage){
				c += vec->storage->header.GetSize();
				for (size_t i=0;i<vec->storage->size;++i){
					c += GetRecursiveSize(vec->storage->objs[i]);
				}
			}
			return c;
		}
		
		case SlangType::Dict: {
			size_t c = obj->GetSize();
			SlangDict* dict = (SlangDict*)obj;
			if (dict->storage){
				c += dict->storage->header.GetSize();
				for (size_t i=0;i<dict->storage->size;++i){
					SlangDictElement* elem = &dict->storage->elements[i];
					if ((uint64_t)elem->key != DICT_UNOCCUPIED_VAL){
						c += GetRecursiveSize(elem->key);
						c += GetRecursiveSize(elem->val);
					}
				}
				c += dict->table->header.GetSize();
			}
			return c;
		}
		
		case SlangType::Maybe: {
			return obj->GetSize()+GetRecursiveSize(((SlangObj*)obj)->maybe);
		}
		
		case SlangType::String: {
			size_t c = obj->GetSize();
			SlangStr* str = (SlangStr*)obj;
			c += str->storage->header.GetSize();
			return c;
		}
		
		case SlangType::Env: {
			size_t c = obj->GetSize();
			SlangEnv* env = (SlangEnv*)obj;
			if (env->next)
				c += GetRecursiveSize((SlangHeader*)env->next);
			return c;
		}
		
		case SlangType::Lambda: {
			size_t c = obj->GetSize();
			SlangLambda* lam = (SlangLambda*)obj;
			c += GetRecursiveSize((SlangHeader*)lam->env);
			c += GetRecursiveSize(lam->expr);
			return c;
		}
		
		case SlangType::InputStream:
		case SlangType::OutputStream: {
			size_t c = obj->GetSize();
			SlangStream* stream = (SlangStream*)obj;
			if (!stream->header.isFile){
				c += GetRecursiveSize((SlangHeader*)stream->str);
			}
			return c;
		}
		
		case SlangType::NullType:
			assert(false);
			return 0;
	}
	return 0;
}

inline bool IsPredefinedSymbol(SymbolName name){
	return name<GLOBAL_SYMBOL_COUNT;
}

inline bool GetRecursiveSymbol(SlangEnv* e,SymbolName name,SlangHeader** val){
	while (e){
		if (e->GetSymbol(name,val))
			return true;
		e = e->parent;
	}
	return false;
}

inline bool ConvertToBool(const SlangHeader* obj){
	if (!obj){
		return false;
	}
	
	switch (obj->type){
		case SlangType::NullType:
		case SlangType::Storage:
		case SlangType::DictTable:
		case SlangType::EndOfFile:
			return false;
		case SlangType::Bool:
			return obj->boolVal;
		case SlangType::Int:
			return (bool)((SlangObj*)obj)->integer;
		case SlangType::Real:
			return (bool)((SlangObj*)obj)->real;
		case SlangType::Maybe:
			return (obj->flags&FLAG_MAYBE_OCCUPIED)!=0;
		case SlangType::Vector: {
			SlangVec* vec = (SlangVec*)obj;
			if (!vec->storage){
				return false;
			} else {
				return vec->storage->size!=0;
			}
		}
		case SlangType::Dict: {
			SlangDict* dict = (SlangDict*)obj;
			if (!dict->storage){
				return false;
			} else {
				return dict->storage->size!=0;
			}
		}
		case SlangType::String: {
			SlangStr* str = (SlangStr*)obj;
			if (!str->storage){
				return false;
			} else {
				return str->storage->size!=0;
			}
		}
		case SlangType::InputStream: {
			SlangStream* stream = (SlangStream*)obj;
			return !stream->IsAtEnd();
		}
		case SlangType::OutputStream:
			return true;
		case SlangType::Params:
			return ((SlangParams*)obj)->size!=0;
		case SlangType::Env:
		case SlangType::Lambda:
		case SlangType::List:
		case SlangType::Symbol:
			return true;
	}
	assert(0);
	return false;
}

size_t ListLen(SlangList* l){
	size_t i=0;
	while (l){
		l = (SlangList*)l->right;
		++i;
		if (GetType(&l->header)!=SlangType::List)
			break;
	}
	return i;
}

inline int64_t floordiv(int64_t a,int64_t b){
	int64_t d = a/b;
	int64_t r = a%b;
	return r ? (d-((a<0)^(b<0))) : d;
}

inline int64_t floormod(int64_t a,int64_t b){
	return a-floordiv(a,b)*b;
}

inline double floorfmod(double a,double b){
	double r = fmod(a,b);
	if (b>0.0)
		return r<0.0 ? r+b : r;
	else
		return r>0.0 ? r+b : r;
}

inline int64_t intpow(int64_t base,uint64_t expo){
	if (expo==0) return 1;
	
	int64_t s = 1;
	while (expo>1){
		if (expo&1){
			s *= base;
			base *= base;
		} else {
			base *= base;
		}
		expo >>= 1;
	}
	return s*base;
}

bool ListEquality(const SlangList* a,const SlangList* b);
bool VectorEquality(const SlangVec* a,const SlangVec* b);
bool DictEquality(const SlangDict* a,const SlangDict* b);
bool StringEquality(const SlangStr* a,const SlangStr* b);

bool EqualObjs(const SlangHeader* a,const SlangHeader* b){
	if (a==b) return true;
	if (!a||!b) return false;
	
	if (a->type==b->type){
		switch (a->type){
			case SlangType::NullType:
			case SlangType::Storage:
			case SlangType::DictTable:
			case SlangType::Lambda:
			case SlangType::Env:
			case SlangType::Params:
				return false;
			case SlangType::List:
				return ListEquality((SlangList*)a,(SlangList*)b);
			case SlangType::Vector:
				return VectorEquality((SlangVec*)a,(SlangVec*)b);
			case SlangType::Dict:
				return DictEquality((SlangDict*)a,(SlangDict*)b);
			case SlangType::String:
				return StringEquality((SlangStr*)a,(SlangStr*)b);
			case SlangType::Maybe:
				if ((a->flags & FLAG_MAYBE_OCCUPIED) != (b->flags & FLAG_MAYBE_OCCUPIED))
					return false;
				return EqualObjs(((SlangObj*)a)->maybe,((SlangObj*)b)->maybe);
			case SlangType::Int:
			case SlangType::Real:
			case SlangType::Symbol:
				// union moment
				return ((SlangObj*)a)->integer==((SlangObj*)b)->integer;
			case SlangType::EndOfFile:
				return true;
			case SlangType::Bool:
				return a->boolVal==b->boolVal;
			case SlangType::InputStream:
			case SlangType::OutputStream:
				if (a->isFile!=b->isFile)
					return false;
				// union moment (covers both str and file)
				return ((SlangStream*)a)->str==((SlangStream*)b)->str;
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

bool DictEquality(const SlangDict* a,const SlangDict* b){
	if (!a->storage || !b->storage){
		return a->storage == b->storage;
	}
	
	if (a->storage->size!=b->storage->size) return false;
	if (a->storage->capacity>b->storage->capacity)
		std::swap(a,b);
	
	for (size_t i=0;i<a->storage->capacity;++i){
		SlangDictElement* elem = &a->storage->elements[i];
		if ((uint64_t)elem->key == DICT_UNOCCUPIED_VAL)
			continue;
			
		SlangHeader* key = elem->key;
		uint64_t aHash = elem->hash;
		SlangHeader* aVal = elem->val;
		SlangHeader* bVal;
		if (!b->LookupKeyWithHash(key,aHash,&bVal))
			return false;
		
		if (!EqualObjs(aVal,bVal))
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

inline bool IdenticalObjs(const SlangHeader* l,const SlangHeader* r){
	if (l==r)
		return true;
	if (!l||!r)
		return false;
	if (l->type!=r->type)
		return false;
	
	// if both are list type, comparing pointers
	// alone is enough to determine if l IS r
	switch (l->type){
		case SlangType::Vector:
		case SlangType::String:
		case SlangType::Dict:
		case SlangType::List:
		case SlangType::NullType:
		case SlangType::Env:
		case SlangType::Params:
		case SlangType::Lambda:
		case SlangType::Storage:
		case SlangType::DictTable:
		case SlangType::InputStream:
		case SlangType::OutputStream:
			return false;
		case SlangType::Maybe:
			if ((l->flags & FLAG_MAYBE_OCCUPIED) != (r->flags & FLAG_MAYBE_OCCUPIED)){
				return false;
			}
			return ((SlangObj*)l)->maybe==((SlangObj*)r)->maybe;
		case SlangType::Int:
		case SlangType::Real:
		case SlangType::Symbol:
			// union moment
			return ((SlangObj*)l)->integer==((SlangObj*)r)->integer;
		case SlangType::EndOfFile:
			return true;
		case SlangType::Bool:
			return l->boolVal==r->boolVal;
	}
	
	return false;
}

inline bool IsValidPathPart(std::string_view s){
	if (s[0]=='.'&&s[1]=='.'&&s.size()==2) return true;
	
	for (char c : s){
		if (c=='/'||c=='\\'||c==' '||c=='?'||c=='*'||
			c=='|'||c=='!'||c=='#'||c=='%'||c=='^'||
			c=='&'||c=='@'||c=='+'||c=='='||c=='`'||
			c=='~'||c=='<'||c=='>'||c=='.')
			return false;
	}
	return true;
}

inline std::string GetBaseDir(const std::string& path){
	size_t lastSlash = path.rfind('/');
	if (lastSlash==std::string::npos) 
		return {};
	return path.substr(0,lastSlash);
}

void PrintInfo(){
	std::cout << "Steps: " << gDebugInterpreter->stepCount << '\n';
#ifndef NDEBUG
	std::cout << "Alloc total: " << gAllocTotal << " (" << gAllocTotal/1024 << " KB)\n";
#endif
	//size_t maxCodeSize = gDebugParser->maxCodeSize;
	//std::cout << "Parse max code size: " << maxCodeSize << " (" << maxCodeSize/1024 << " KB)\n";
	std::cout << "Symbol name collisions: " << gNameCollisions << '\n';
	std::cout << "Total symbol count: " << gDebugParser->currentName << '\n';
	size_t codeAlloc = gDebugInterpreter->codeWriter.totalAlloc;
	std::cout << "Code alloc: " << codeAlloc << " (" << codeAlloc/1024 << " KB)\n";
	size_t locSize = gDebugInterpreter->codeWriter.GetCodeLocationsSize();
	std::cout << "Code location mem: " << locSize << " (" << locSize/1024 << " KB)\n";
	std::cout << "Max stack height: " << gMaxStackHeight << '\n';
	std::cout << "Small GCs: " << gSmallGCs << '\n';
	std::cout << "Realloc count: " << gReallocCount << '\n';
	std::cout << "Max arena size: " << gMaxArenaSize/1024 << " KB\n";
	std::cout << "Curr arena size: " << gArenaSize/1024 << " KB\n";
	std::cout << "Case misses: " << gCaseMisses << "\n";
	size_t caseMem = gDebugInterpreter->codeWriter.caseDictElements.cap*sizeof(CaseDictElement);
	std::cout << "Case mem: " << caseMem/1024 << " KB\n";
	std::cout << std::fixed << std::setprecision(3);
	std::cout << "Parse time: " << gParseTime*1000 << "ms\n";
	std::cout << "Compile time: " << gCompileTime*1000 << "ms\n";
	if (gRunTime>=2.0)
		std::cout << "Run time: " << gRunTime << "s\n";
	else
		std::cout << "Run Time: " << gRunTime*1000 << "ms\n";
	double stepsPerSecond = (double)gDebugInterpreter->stepCount/gRunTime/1000.0;
	if (stepsPerSecond>=2000.0)
		std::cout << "Steps/s: " << stepsPerSecond/1000.0 << "M/s\n";
	else
		std::cout << "Steps/s: " << stepsPerSecond << "K/s\n";
}

inline bool IsWhitespace(char c){
	return c==' '||c=='\t'||c=='\n';
}

inline bool IsNumber(char c){
	return c>='0'&&c<='9';
}

inline void SlangTokenizer::TokenizeNumber(SlangToken& token){
	bool neg = *pos=='-';
	if (neg) ++pos;
	token.type = (*pos=='.') ? SlangTokenType::Real : SlangTokenType::Int;
	++pos;
	// got "-."
	if (neg&&token.type==SlangTokenType::Real&&!IsNumber(*pos)){
		parser->PushError("'-.' is not a valid token!");
		token.type = SlangTokenType::Error;
		++pos;
		return;
	}
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
		std::stringstream msg{};
		msg << "Unexpected char in number: '" << *pos << "'";
		parser->PushError(msg.str());
		token.type = SlangTokenType::Error;
		++pos;
	}
}

inline void SlangTokenizer::TokenizeIdentifier(SlangToken& token){
	token.type = SlangTokenType::Symbol;
	while (pos!=end&&IsIdentifierChar(*pos)){
		++pos;
	}
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
	
	switch (c){
		case '(':
		case '[':
		case '{':
			token.type = SlangTokenType::LeftBracket;
			++pos;
			break;
		case ')':
		case ']':
		case '}':
			token.type = SlangTokenType::RightBracket;
			++pos;
			break;
		case '#':
			if (nextC=='('||nextC=='['||nextC=='{'){
				token.type = SlangTokenType::VectorMarker;
				++pos;
			} else {
				token.type = SlangTokenType::Error;
				++pos;
			}
			break;
		case '\'':
			token.type = SlangTokenType::Quote;
			++pos;
			break;
		case '`':
			token.type = SlangTokenType::Quasiquote;
			++pos;
			break;
		case ',':
			token.type = SlangTokenType::Unquote;
			++pos;
			break;
		case '@':
			token.type = SlangTokenType::UnquoteSplicing;
			++pos;
			break;
		case '!':
			if (nextC!='='){
				token.type = SlangTokenType::Not;
				++pos;
			} else {
				TokenizeIdentifier(token);
			}
			break;
		case '/':
			if (nextC!='/'&&IsIdentifierChar(nextC)){
				token.type = SlangTokenType::Invert;
				++pos;
			} else {
				TokenizeIdentifier(token);
			}
			break;
		case '-':
			if (nextC!='-'&&
				(IsIdentifierChar(nextC)||nextC=='('||nextC=='['||nextC=='{')&&
				!IsNumber(nextC)&&nextC!='.'){
				token.type = SlangTokenType::Negation;
				++pos;
			} else if (nextC=='-'){
				TokenizeIdentifier(token);
			} else if (IsNumber(nextC)||nextC=='.'){
				TokenizeNumber(token);
			} else {
				TokenizeIdentifier(token);
			}
			break;
		case '"':
			token.type = SlangTokenType::String;
			++pos;
			while (pos!=end&&*pos!='"'){
				if (*pos=='\n'){
					parser->PushError("Expected '\"', not '\\n'");
					token.type = SlangTokenType::Error;
					break;
				} else if (*pos=='\\'){
					++pos;
				}
				++pos;
			}
			if (pos==end){
				parser->PushError("Expected '\"', not EOF");
				token.type = SlangTokenType::Error;
			} else {
				++pos;
			}
			break;
		case ';':
			if (nextC=='-'){
				// block comment
				token.type = SlangTokenType::Comment;
				char lastC = ' ';
				++pos;
				++pos;
				while (pos!=end){
					if (*pos==';'&&lastC=='-'){
						++pos;
						break;
					}
					if (*pos=='\n') ++line;
					lastC = *pos;
					++pos;
				}
			} else {
				// line comment
				token.type = SlangTokenType::Comment;
				while (pos!=end&&*pos!='\n') ++pos;
			}
			break;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			TokenizeNumber(token);
			break;
		case '.':
			if (IsNumber(nextC)){
				TokenizeNumber(token);
			} else if (IsWhitespace(nextC)){
				token.type = SlangTokenType::Dot;
				++pos;
			} else {
				TokenizeIdentifier(token);
			}
			break;
		default:
			if (!IsIdentifierChar(c)){
				token.type = SlangTokenType::Error;
				++pos;
				break;
			}
			
			TokenizeIdentifier(token);
			break;
	}
	
	token.view = {begin,pos};
	if (token.type==SlangTokenType::Symbol){
		if (token.view=="true"){
			token.type = SlangTokenType::True;
		} else if (token.view=="false"){
			token.type = SlangTokenType::False;
		}
	}
	col += pos-begin;
	return token;
}

SymbolName SlangParser::RegisterSymbol(std::string_view str){
	SymbolName find = nameDict.Find(str);
	if (find!=-1ULL)
		return find;
	
	SymbolName name = currentName++;
	size_t strStart = nameStringStorage.size;
	nameStringStorage.AddSize(str.size());
	memcpy(nameStringStorage.data+strStart,str.data(),str.size());
	StringLocation& loc = symbolToStringArray.PlaceBack();
	loc.start = strStart;
	loc.count = str.size();
	nameDict.Insert(loc,name);
	return name;
}

std::string_view SlangParser::GetSymbolString(SymbolName symbol){
	if (symbol==LET_SELF_SYM)
		return "$letself";
	if (symbol==EMPTY_NAME)
		return "$EMPTY";
		
	if (symbol & GEN_FLAG){
		std::stringstream ss{};
		ss << "$tmp" << ((uint64_t)symbol&~GEN_FLAG);
		return ss.str();
	}
	
	if (symbol>symbolToStringArray.size)
		return "UNDEFINED";
	StringLocation loc = symbolToStringArray.data[symbol];
	const char* s = (const char*)(nameStringStorage.data+loc.start);
	std::string_view sv{s,loc.count};
	return sv;
}

void SlangParser::PushError(const std::string& msg){
	LocationData l = {token.line,token.col,currModule};
	errors.emplace_back(l,"SyntaxError",msg);
}

inline void SlangParser::NextToken(){
	if (!errors.empty()) return;
	if (token.type==SlangTokenType::EndOfFile){
		PushError("Ran out of tokens!");
		return;
	}
	
	token = tokenizer->NextToken();
	while (token.type==SlangTokenType::Comment){
		token = tokenizer->NextToken();
	}
}

inline void* SlangParserCodeAlloc(void* d,size_t size){
	MemChain* p = (MemChain*)d;
	void* data = p->Allocate(size);
	return data;
}

inline SlangList* SlangParser::WrapExprIn(SymbolName func,SlangHeader* expr){
	SlangObj* q = alloc.MakeSymbol(func);

	SlangList* list = alloc.AllocateList();
	list->left = (SlangHeader*)q;
	SlangList* listright = alloc.AllocateList();
	listright->left = expr;
	listright->right = nullptr;
	list->right = (SlangHeader*)listright;

	return list;
}

inline SlangList* SlangParser::WrapExprSplice(SymbolName func,SlangList* expr){
	SlangObj* q = alloc.MakeSymbol(func);

	SlangList* list = alloc.AllocateList();
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

inline bool BracketsMatch(char l,char r){
	switch (l){
		case '(':
			return r==')';
		case '[':
			return r==']';
		case '{':
			return r=='}';
	}
	return false;
}

inline bool SlangParser::ParseVec(SlangHeader** res){
	char leftB = token.view[0];
	uint32_t line = token.line;
	uint32_t col = token.col;
	assert(token.type==SlangTokenType::LeftBracket);
	NextToken();
	
	Vector<SlangHeader*> buildVec{};
	buildVec.Reserve(8);
	while (token.type!=SlangTokenType::RightBracket){
		if (token.type==SlangTokenType::EndOfFile){
			PushError("Expected ')'! (from "+std::to_string(line+1)+","+std::to_string(col+1)+")");
			return false;
		}
		
		SlangHeader* curr;
		if (!ParseObj(&curr)) return false;
		
		buildVec.PushBack(curr);
	}
	
	if (!BracketsMatch(leftB,token.view[0])){
		std::string start = {},end = {};
		start += token.view[0];
		switch (leftB){
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
	
	SlangVec* v = alloc.AllocateVec(buildVec.size);
	if (buildVec.size!=0)
		memcpy(v->storage->data,buildVec.data,sizeof(SlangHeader*)*buildVec.size);
	*res = (SlangHeader*)v;
	NextToken();
	return true;
}

inline bool SlangParser::ParseObj(SlangHeader** res){
	LocationData loc = {token.line,token.col,currModule};
	switch (token.type){
		case SlangTokenType::Int: {
			char* d;
			int64_t i = strtoll(token.view.begin(),&d,10);
			NextToken();
			SlangObj* intObj = alloc.MakeInt(i);
			codeMap[(SlangHeader*)intObj] = loc;
			*res = (SlangHeader*)intObj;
			return true;
		}
		case SlangTokenType::Real: {
			char* end;
			errno = 0;
			double r = strtod(token.view.begin(),&end);
			NextToken();
			if (errno==ERANGE){
				PushError("Could not parse real: "+std::string(token.view));
				return false;
			}
			SlangObj* realObj = alloc.MakeReal(r);
			codeMap[(SlangHeader*)realObj] = loc;
			*res = (SlangHeader*)realObj;
			return true;
		}
		case SlangTokenType::Symbol: {
			SymbolName s;
			s = RegisterSymbol(token.view);
			NextToken();
			SlangObj* sym = alloc.MakeSymbol(s);
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
			SlangStr* strObj = alloc.AllocateStr(val.size());
			if (!val.empty())
				strObj->CopyFromString(val);
			codeMap[(SlangHeader*)strObj] = loc;
			*res = (SlangHeader*)strObj;
			return true;
		}
		case SlangTokenType::VectorMarker: {
			NextToken();
			SlangHeader* vecObj;
			if (!ParseVec(&vecObj)) return false;
			
			codeMap[vecObj] = loc;
			*res = vecObj;
			return true;
		}
		case SlangTokenType::True: {
			NextToken();
			SlangHeader* boolObj = alloc.MakeBool(true);
			codeMap[boolObj] = loc;
			*res = boolObj;
			return true;
		}
		case SlangTokenType::False: {
			NextToken();
			SlangHeader* boolObj = alloc.MakeBool(false);
			codeMap[boolObj] = loc;
			*res = boolObj;
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
		case SlangTokenType::Quasiquote: {
			NextToken();
			SlangHeader* sub;
			if (!ParseObj(&sub)) return false;
			*res = (SlangHeader*)WrapExprIn(SLANG_QUASIQUOTE,sub);
			codeMap[*res] = loc;
			return true;
		}
		case SlangTokenType::Unquote: {
			NextToken();
			SlangHeader* sub;
			if (!ParseObj(&sub)) return false;
			*res = (SlangHeader*)WrapExprIn(SLANG_UNQUOTE,sub);
			codeMap[*res] = loc;
			return true;
		}
		case SlangTokenType::UnquoteSplicing: {
			NextToken();
			SlangHeader* sub;
			if (!ParseObj(&sub)) return false;
			*res = (SlangHeader*)WrapExprIn(SLANG_UNQUOTE_SPLICING,sub);
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
			*res = (SlangHeader*)WrapExprIn(SLANG_SUB,sub);
			codeMap[*res] = loc;
			return true;
		}
		case SlangTokenType::Invert: {
			NextToken();
			SlangHeader* sub;
			if (!ParseObj(&sub)) return false;
			*res = (SlangHeader*)WrapExprIn(SLANG_DIV,sub);
			codeMap[*res] = loc;
			return true;
		}
		case SlangTokenType::LeftBracket: {
			if (!ParseExpr(res)) return false;
			codeMap[*res] = loc;
			return true;
		}
		case SlangTokenType::RightBracket:
		case SlangTokenType::Dot:
		case SlangTokenType::Comment:
		case SlangTokenType::Error:
			PushError("Unexpected token: "+std::string(token.view));
			return false;
		case SlangTokenType::EndOfFile:
			// might be superfluous
			PushError("Ran out of tokens!");
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
				SlangList* newHead = alloc.AllocateList();
				newHead->left = (SlangHeader*)doWrapper;
				newHead->right = nullptr;
				nextnext->right = (SlangHeader*)newHead;
			} else {
				// (let (()) b1 b2...)
				SlangList* doWrapper = WrapExprSplice(SLANG_DO,(SlangList*)next->right);
				SlangList* newHead = alloc.AllocateList();
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
			SlangList* newHead = alloc.AllocateList();
			newHead->left = (SlangHeader*)doWrapper;
			newHead->right = nullptr;
			next->right = (SlangHeader*)newHead;
			return;
		}
		case SLANG_DEFINE: {
			// (def (func arg1 arg2...) expr1 expr2...) ->
			// (def func (& (arg1 arg2...) (do expr1 expr2...)))
			if (argCount<3) return;
			SlangList* next = (SlangList*)list->right;
			if (GetType(next->left)!=SlangType::List)
				return;
			
			SlangList* args = (SlangList*)next->left;
			SlangHeader* funcName = args->left;
			SlangList* exprsStart = (SlangList*)next->right;
			
			SlangList* lambdaWrapper = WrapExprSplice(SLANG_LAMBDA,exprsStart);
			SlangList* inbetween = alloc.AllocateList();
			inbetween->right = lambdaWrapper->right;
			lambdaWrapper->right = (SlangHeader*)inbetween;
			inbetween->left = args->right;
			next->left = funcName;
			SlangList* lambdaList = alloc.AllocateList();
			lambdaList->right = nullptr;
			lambdaList->left = (SlangHeader*)lambdaWrapper;
			next->right = (SlangHeader*)lambdaList;
			TestSeqMacro(lambdaWrapper,GetArgCount(lambdaWrapper));
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
	uint32_t line = token.line;
	uint32_t col = token.col;
	
	NextToken();
	
	while (token.type!=SlangTokenType::RightBracket){
		if (token.type==SlangTokenType::Dot){
			NextToken();
			if (!ParseObj(&obj->right))
				return false;
			
			if (token.type!=SlangTokenType::RightBracket){
				PushError("Expected end of list after dot, not "+std::string(token.view));
				return false;
			}
			break;
		}
		if (token.type==SlangTokenType::EndOfFile){
			PushError("Expected ')'! (from "+std::to_string(line+1)+","+std::to_string(col+1)+")");
			return false;
		}
		
		if (!obj){
			obj = alloc.AllocateList();
			*res = (SlangHeader*)obj;
		} else {
			obj->right = (SlangHeader*)alloc.AllocateList();
			obj = (SlangList*)obj->right;
		}
		if (!ParseObj(&obj->left))
			return false;
		
		obj->right = nullptr;
		++argCount;
	}
	
	char rightBracket = token.view[0];
	if (!BracketsMatch(leftBracket,rightBracket)){
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

bool SlangParser::ParseLine(SlangHeader** res){
	codeMap.clear();
	memChain.Reset();
	return ParseObj(res);
}

void SlangParser::SetCodeString(std::string_view code){
	tokenizer = std::make_unique<SlangTokenizer>(code,this);
	gDebugParser = this;
	errors.clear();
	
	token.type = SlangTokenType::Comment;
	NextToken();
}

inline LocationData SlangParser::GetExprLocation(const SlangHeader* expr){
	if (!codeMap.contains(expr))
		return {-1U,-1U,-1U};
	
	return codeMap.at(expr);
}

SlangParser::SlangParser() :
		tokenizer(),token(),
		nameStringStorage(),
		nameDict(nameStringStorage),
		currentName(0),errors(),
		alloc(&memChain,SlangParserCodeAlloc),memChain(8192){
	nameDict.Reserve(512);
	nameStringStorage.Reserve(4096);
	symbolToStringArray.Reserve(256);
	for (const auto& k : gDefaultNameArray){
		RegisterSymbol(k);
	}
	
	token.type = SlangTokenType::Comment;
	maxCodeSize = 0;
	codeMap.reserve(512);
	currModule = 0;
}

void CodeWriter::AddCodeLocation(const SlangHeader* expr){
	LocationData loc = parser.GetExprLocation(expr);
	CodeLocationPair& cl = curr->locData.PlaceBack();
	cl.codeIndex = curr->write-curr->start;
	cl.line = loc.line;
	cl.col = loc.col;
}

LocationData CodeWriter::FindCodeLocation(size_t funcIndex,size_t offset) const {
	const Vector<CodeLocationPair>& locData = lambdaCodes[funcIndex].locData;
	LocationData res{-1U,-1U,lambdaCodes[funcIndex].moduleIndex};
	for (size_t i=0;i<locData.size;++i){
		if ((size_t)locData.data[i].codeIndex>=offset){
			res.line = locData.data[i].line;
			res.col = locData.data[i].col;
			break;
		}
	}
	return res;
}

size_t CodeWriter::GetCodeLocationsSize() const {
	size_t s = 0;
	for (const auto& block : lambdaCodes){
		s += sizeof(CodeLocationPair)*block.locData.cap;
	}
	return s;
}

void CodeWriter::PushError(const SlangHeader* expr,const std::string& type,const std::string& msg){
	LocationData loc = parser.GetExprLocation(expr);
	errors.emplace_back(loc,type,msg);
}

void CodeWriter::TypeError(const SlangHeader* expr,SlangType found,SlangType expected){
	std::stringstream msg = {};
	msg << "Expected type " << 
		TypeToString(expected) << " instead of type " << 
		TypeToString(found) << " in argument '";
	if (expr)
		msg << *expr << "'";
	else
		msg << "()'";
		
	PushError(expr,"TypeError",msg.str());
}

void CodeWriter::ReservedError(const SlangHeader* expr,SymbolName sym){
	std::stringstream msg = {};
	msg << "Cannot assign to reserved keyword '" << 
		parser.GetSymbolString(sym) << "' in expr '";
	if (expr)
		msg << *expr << "'";
	else
		msg << "()'";
		
	PushError(expr,"ReservedError",msg.str());
}

void CodeWriter::RedefinedError(const SlangHeader* expr,SymbolName sym){
	std::stringstream msg = {};
	msg << "Symbol '" << 
		parser.GetSymbolString(sym) << "' was defined twice in expr '";
	if (expr)
		msg << *expr << "'";
	else
		msg << "()'";
		
	PushError(expr,"RedefinedError",msg.str());
}

void CodeWriter::FuncRedefinedError(const SlangHeader* expr,SymbolName sym){
	std::stringstream ss{};
	ss << "Function '";
	ss << parser.GetSymbolString(sym);
	ss << "' was redefined!";
	PushError(expr,"RedefinedError",ss.str());
}

void CodeWriter::LetRecError(const SlangHeader* expr,SymbolName sym){
	std::stringstream msg = {};
	msg << "Symbol '" << 
		parser.GetSymbolString(sym) << "' is not defined yet!";
		
	PushError(expr,"LetRecError",msg.str());
}

void CodeWriter::DefError(const SlangHeader* expr){
	std::stringstream msg = {};
	msg << "Can only use 'def' in the global scope";
		
	PushError(expr,"DefError",msg.str());
}

void CodeWriter::ArityError(const SlangHeader* head,size_t found,size_t expectMin,size_t expectMax){
	std::stringstream msg = {};
	const char* plural = (expectMin!=1) ? "s" : "";
	
	msg << "Procedure '";
	if (head)
		msg << *head;
	else
		msg << "()";
		
	if (expectMin==expectMax){
		msg << "' takes " << expectMin << " argument" << plural << ", was given " <<
			found;
	} else if (expectMax==VARIADIC_ARG_COUNT){
		msg << "' takes at least " << expectMin << " argument" << plural << ", was given " <<
			found;
	} else {
		msg << "' takes between " << expectMin << " and " << expectMax <<
			" arguments, was given " << found;
	}
		
	PushError(head,"ArityError",msg.str());
}

void CodeWriter::DotError(const SlangHeader* expr){
	std::stringstream msg = {};
	msg << "Expected List instead of DottedList";
	PushError(expr,"TypeError",msg.str());
}

inline void* CodeWriterObjAlloc(void* data,size_t s){
	MemChain* c = (MemChain*)data;
	void* p = c->Allocate(s);
	return p;
}

SlangHeader* CodeWriter::Copy(const SlangHeader* obj){
	if (!obj)
		return nullptr;
	
	switch (obj->type){
		case SlangType::Int:
		case SlangType::Real:
		case SlangType::Symbol:
		case SlangType::EndOfFile:
		case SlangType::Bool: {
			SlangHeader* newObj = (SlangHeader*)alloc.Allocate(obj->GetSize());
			memcpy(newObj,obj,obj->GetSize());
			return newObj;
		}
		case SlangType::List: {
			SlangList* oldList = (SlangList*)obj;
			SlangList* newList = (SlangList*)alloc.Allocate(obj->GetSize());
			newList->header = oldList->header;
			newList->left = Copy(oldList->left);
			newList->right = Copy(oldList->right);
			return (SlangHeader*)newList;
		}
		case SlangType::Maybe: {
			SlangObj* oldMaybe = (SlangObj*)obj;
			SlangObj* newMaybe = (SlangObj*)alloc.Allocate(obj->GetSize());
			newMaybe->header = oldMaybe->header;
			newMaybe->maybe = Copy(oldMaybe->maybe);
			return (SlangHeader*)newMaybe;
		}
		case SlangType::Vector: {
			SlangVec* oldVec = (SlangVec*)obj;
			SlangVec* newVec = (SlangVec*)alloc.Allocate(obj->GetSize());
			memcpy(newVec,oldVec,obj->GetSize());
			if (!oldVec->storage)
				return (SlangHeader*)newVec;
			newVec->storage = (SlangStorage*)Copy((SlangHeader*)oldVec->storage);
			return (SlangHeader*)newVec;
		}
		case SlangType::String: {
			SlangStr* oldStr = (SlangStr*)obj;
			SlangStr* newStr = (SlangStr*)alloc.Allocate(obj->GetSize());
			memcpy(newStr,oldStr,obj->GetSize());
			if (!oldStr->storage)
				return (SlangHeader*)newStr;
			newStr->storage = (SlangStorage*)Copy((SlangHeader*)oldStr->storage);
			return (SlangHeader*)newStr;
		}
		case SlangType::Dict: {
			SlangDict* oldDict = (SlangDict*)obj;
			SlangDict* newDict = (SlangDict*)alloc.Allocate(obj->GetSize());
			memcpy(newDict,oldDict,obj->GetSize());
			if (!oldDict->storage)
				return (SlangHeader*)newDict;
			newDict->table = (SlangDictTable*)Copy((SlangHeader*)oldDict->table);
			newDict->storage = (SlangStorage*)Copy((SlangHeader*)oldDict->storage);
			return (SlangHeader*)newDict;
		}
		case SlangType::Storage: {
			SlangStorage* oldStorage = (SlangStorage*)obj;
			SlangStorage* newStorage = alloc.AllocateStorage(oldStorage->size,obj->elemSize);
			if (obj->elemSize==sizeof(uint8_t)){
				memcpy(newStorage->data,oldStorage->data,oldStorage->size);
				return (SlangHeader*)newStorage;
			}
			
			if (obj->elemSize==sizeof(SlangHeader*)){
				for (size_t i=0;i<oldStorage->size;++i){
					newStorage->objs[i] = Copy(oldStorage->objs[i]);
				}
				return (SlangHeader*)newStorage;
			}
			
			if (obj->elemSize==sizeof(SlangDictElement)){
				memcpy(
					newStorage->elements,
					oldStorage->elements,
					sizeof(SlangDictElement)*oldStorage->size
				);
				for (size_t i=0;i<oldStorage->size;++i){
					if ((uint64_t)newStorage->elements[i].key!=DICT_UNOCCUPIED_VAL){
						newStorage->elements[i].key = Copy(oldStorage->elements[i].key);
						newStorage->elements[i].val = Copy(oldStorage->elements[i].val);
					}
				}
				return (SlangHeader*)newStorage;
			}
			return (SlangHeader*)newStorage;
		}
		case SlangType::DictTable: {
			SlangDictTable* oldTable = (SlangDictTable*)obj;
			SlangDictTable* newTable = alloc.AllocateDictTable(oldTable->capacity);
			memcpy(newTable,oldTable,obj->GetSize());
			return (SlangHeader*)newTable;
		}
		
		default:
			assert(false);
			return nullptr;
	}
}

inline void CodeWriter::ReallocCurrBlock(size_t newSize){
	size_t offset = curr->write-curr->start;
	size_t lastInstOff = lastInst-curr->start;
	totalAlloc += newSize-curr->size;
	curr->start = (uint8_t*)realloc(curr->start,newSize);
	curr->write = curr->start+offset;
	curr->size = newSize;
	lastInst = curr->start+lastInstOff;
}

#define CODE_REALLOC_BLOCK(type) \
	if (curr->write-curr->start+sizeof(type) > curr->size){ \
		ReallocCurrBlock(curr->size*3/2 + sizeof(type)); \
	}
	
#define ARITY_CHECK_EXACT(expr,count,arity) \
	if ((count)!=(arity)){ \
		ArityError((expr),(count),(arity),(arity)); \
		return false; \
	}

enum SlangOpCodes {
	SLANG_OP_NOOP,
	SLANG_OP_HALT,
	// push literals
	SLANG_OP_NULL,
	SLANG_OP_LOAD_PTR,
	SLANG_OP_BOOL_TRUE,
	SLANG_OP_BOOL_FALSE,
	SLANG_OP_ZERO,
	SLANG_OP_ONE,
	SLANG_OP_PUSH_LAMBDA,
	
	// variable ops
	SLANG_OP_LOOKUP,
	SLANG_OP_SET,
	SLANG_OP_GET_LOCAL,
	SLANG_OP_SET_LOCAL,
	SLANG_OP_GET_GLOBAL,
	SLANG_OP_SET_GLOBAL,
	SLANG_OP_DEF_GLOBAL,
	SLANG_OP_GET_REC,
	SLANG_OP_SET_REC,
	
	// arg manip
	SLANG_OP_PUSH_FRAME,
	SLANG_OP_POP_ARG,
	SLANG_OP_UNPACK,
	SLANG_OP_COPY,
	// functions calls
	SLANG_OP_CALL,
	SLANG_OP_CALLSYM,
	SLANG_OP_RET,
	SLANG_OP_RETCALL,
	SLANG_OP_RETCALLSYM,
	SLANG_OP_RECURSE,
	// jumps
	SLANG_OP_JUMP,
	SLANG_OP_CJUMP_POP,
	SLANG_OP_CNJUMP_POP,
	SLANG_OP_CJUMP,
	SLANG_OP_CNJUMP,
	SLANG_OP_CASE_JUMP,
	SLANG_OP_TRY,
	SLANG_OP_MAYBE_NULL,
	SLANG_OP_MAYBE_WRAP,
	SLANG_OP_MAYBE_UNWRAP,
	
	SLANG_OP_MAP_STEP,
	
	SLANG_OP_NOT,
	// simple math
	SLANG_OP_INC,
	SLANG_OP_DEC,
	SLANG_OP_NEG,
	SLANG_OP_INVERT,
	SLANG_OP_ADD,
	SLANG_OP_SUB,
	SLANG_OP_MUL,
	SLANG_OP_DIV,
	SLANG_OP_EQ,
	// list ops
	SLANG_OP_PAIR,
	SLANG_OP_LIST_CONCAT,
	SLANG_OP_LEFT,
	SLANG_OP_RIGHT,
	SLANG_OP_SET_LEFT,
	SLANG_OP_SET_RIGHT,
	
	// vec ops
	SLANG_OP_MAKE_VEC,
	SLANG_OP_VEC_GET,
	SLANG_OP_VEC_SET,
	
	SLANG_OP_EXPORT,
	SLANG_OP_IMPORT,
	
	SLANG_OP_COUNT
};

static_assert(SLANG_OP_COUNT<=256);

#define OPCODE_SIZE 1
static const size_t SlangOpSizes[] = {
	OPCODE_SIZE,    // SLANG_OP_NOOP
	OPCODE_SIZE,    // SLANG_OP_HALT
	OPCODE_SIZE,    // SLANG_OP_NULL
	OPCODE_SIZE+8,  // SLANG_OP_LOAD_PTR
	OPCODE_SIZE,    // SLANG_OP_BOOL_TRUE
	OPCODE_SIZE,    // SLANG_OP_BOOL_FALSE
	OPCODE_SIZE,    // SLANG_OP_ZERO
	OPCODE_SIZE,    // SLANG_OP_ONE
	OPCODE_SIZE+8,  // SLANG_OP_PUSH_LAMBDA
	OPCODE_SIZE+8,  // SLANG_OP_LOOKUP
	OPCODE_SIZE+8,  // SLANG_OP_SET
	OPCODE_SIZE+2,  // SLANG_OP_GET_LOCAL
	OPCODE_SIZE+2,  // SLANG_OP_SET_LOCAL
	OPCODE_SIZE+8,  // SLANG_OP_GET_GLOBAL
	OPCODE_SIZE+8,  // SLANG_OP_SET_GLOBAL
	OPCODE_SIZE+8,  // SLANG_OP_DEF_GLOBAL
	OPCODE_SIZE+2,  // SLANG_OP_GET_REC
	OPCODE_SIZE+2,  // SLANG_OP_SET_REC
	OPCODE_SIZE,    // SLANG_OP_PUSH_FRAME
	OPCODE_SIZE,    // SLANG_OP_POP_ARG
	OPCODE_SIZE,    // SLANG_OP_UNPACK
	OPCODE_SIZE,    // SLANG_OP_COPY
	OPCODE_SIZE,    // SLANG_OP_CALL
	OPCODE_SIZE+2,  // SLANG_OP_CALLSYM
	OPCODE_SIZE,    // SLANG_OP_RET
	OPCODE_SIZE,    // SLANG_OP_RETCALL
	OPCODE_SIZE+2,  // SLANG_OP_RETCALLSYM
	OPCODE_SIZE,    // SLANG_OP_RECURSE
	OPCODE_SIZE+4,  // SLANG_OP_JUMP
	OPCODE_SIZE+4,  // SLANG_OP_CJUMP_POP
	OPCODE_SIZE+4,  // SLANG_OP_CNJUMP_POP
	OPCODE_SIZE+4,  // SLANG_OP_CJUMP
	OPCODE_SIZE+4,  // SLANG_OP_CNJUMP
	OPCODE_SIZE+4,  // SLANG_OP_CASE_JUMP
	OPCODE_SIZE+4,  // SLANG_OP_TRY
	OPCODE_SIZE,    // SLANG_OP_MAYBE_NULL
	OPCODE_SIZE,    // SLANG_OP_MAYBE_WRAP
	OPCODE_SIZE,    // SLANG_OP_MAYBE_UNWRAP
	OPCODE_SIZE+2,  // SLANG_OP_MAP_STEP
	OPCODE_SIZE,    // SLANG_OP_NOT
	OPCODE_SIZE,    // SLANG_OP_INC
	OPCODE_SIZE,    // SLANG_OP_DEC
	OPCODE_SIZE,    // SLANG_OP_NEG
	OPCODE_SIZE,    // SLANG_OP_INVERT
	OPCODE_SIZE+2,  // SLANG_OP_ADD
	OPCODE_SIZE+2,  // SLANG_OP_SUB
	OPCODE_SIZE+2,  // SLANG_OP_MUL
	OPCODE_SIZE+2,  // SLANG_OP_DIV
	OPCODE_SIZE,    // SLANG_OP_EQ
	OPCODE_SIZE,    // SLANG_OP_PAIR
	OPCODE_SIZE+2,  // SLANG_OP_LIST_CONCAT
	OPCODE_SIZE,    // SLANG_OP_LEFT
	OPCODE_SIZE,    // SLANG_OP_RIGHT
	OPCODE_SIZE,    // SLANG_OP_SET_LEFT
	OPCODE_SIZE,    // SLANG_OP_SET_RIGHT
	OPCODE_SIZE,    // SLANG_OP_MAKE_VEC
	OPCODE_SIZE,    // SLANG_OP_VEC_GET
	OPCODE_SIZE,    // SLANG_OP_VEC_SET
	OPCODE_SIZE+8,  // SLANG_OP_EXPORT
	OPCODE_SIZE+8,  // SLANG_OP_IMPORT
};
static_assert(SL_ARR_LEN(SlangOpSizes)==SLANG_OP_COUNT);	

inline void CodeWriter::WriteOpCode(uint8_t op){
	CODE_REALLOC_BLOCK(uint8_t);
	lastInst = curr->write;
	*curr->write++ = op;
}

inline void CodeWriter::WriteInt16(uint16_t i){
	CODE_REALLOC_BLOCK(uint16_t);
	*curr->write++ = i&0xFF;
	i >>= 8;
	*curr->write++ = i&0xFF;
}

inline void CodeWriter::WriteInt32(uint32_t i){
	CODE_REALLOC_BLOCK(uint32_t);
	*curr->write++ = i&0xFF;
	i >>= 8;
	*curr->write++ = i&0xFF;
	i >>= 8;
	*curr->write++ = i&0xFF;
	i >>= 8;
	*curr->write++ = i&0xFF;
}

inline void CodeWriter::WriteSInt32(int32_t i){
	CODE_REALLOC_BLOCK(int32_t);
	*curr->write++ = i&0xFF;
	i >>= 8;
	*curr->write++ = i&0xFF;
	i >>= 8;
	*curr->write++ = i&0xFF;
	i >>= 8;
	*curr->write++ = i&0xFF;
}

inline void CodeWriter::WriteInt64(uint64_t val){
	CODE_REALLOC_BLOCK(uint64_t);
	*curr->write++ = val&0xFF;
	val >>= 8;
	*curr->write++ = val&0xFF;
	val >>= 8;
	*curr->write++ = val&0xFF;
	val >>= 8;
	*curr->write++ = val&0xFF;
	val >>= 8;
	*curr->write++ = val&0xFF;
	val >>= 8;
	*curr->write++ = val&0xFF;
	val >>= 8;
	*curr->write++ = val&0xFF;
	val >>= 8;
	*curr->write++ = val&0xFF;
}

inline void CodeWriter::WritePointer(const void* ptr){
	CODE_REALLOC_BLOCK(uint64_t);
	uint64_t val = (uint64_t)ptr;
	*curr->write++ = val&0xFF;
	val >>= 8;
	*curr->write++ = val&0xFF;
	val >>= 8;
	*curr->write++ = val&0xFF;
	val >>= 8;
	*curr->write++ = val&0xFF;
	val >>= 8;
	*curr->write++ = val&0xFF;
	val >>= 8;
	*curr->write++ = val&0xFF;
	val >>= 8;
	*curr->write++ = val&0xFF;
	val >>= 8;
	*curr->write++ = val&0xFF;
}

inline void CodeWriter::WriteSymbolName(SymbolName sym){
	CODE_REALLOC_BLOCK(SymbolName);
	*curr->write++ = sym&0xFF;
	sym >>= 8;
	*curr->write++ = sym&0xFF;
	sym >>= 8;
	*curr->write++ = sym&0xFF;
	sym >>= 8;
	*curr->write++ = sym&0xFF;
	sym >>= 8;
	*curr->write++ = sym&0xFF;
	sym >>= 8;
	*curr->write++ = sym&0xFF;
	sym >>= 8;
	*curr->write++ = sym&0xFF;
	sym >>= 8;
	*curr->write++ = sym&0xFF;
}

inline size_t CodeWriter::WriteStartJump(){
	size_t offset = curr->write-curr->start;
	assert(offset<INT32_MAX);
	WriteOpCode(SLANG_OP_JUMP);
	// 4 NOOPs
	WriteSInt32(0);
	return offset;
}

inline void CodeWriter::FinishJump(size_t offset){
	int32_t dist = curr->write-curr->start-offset;
	uint8_t* start = curr->start+offset;
	start++;
	*start++ = dist&0xFF;
	dist >>= 8;
	*start++ = dist&0xFF;
	dist >>= 8;
	*start++ = dist&0xFF;
	dist >>= 8;
	*start++ = dist&0xFF;
}

inline size_t CodeWriter::WriteStartCJump(bool jumpOn){
	size_t offset = curr->write-curr->start;
	assert(offset<INT32_MAX);
	if (jumpOn){
		WriteOpCode(SLANG_OP_CNJUMP);
	} else {
		WriteOpCode(SLANG_OP_CJUMP);
	}
	// 4 NOOPs
	WriteSInt32(0);
	return offset;
}

inline size_t CodeWriter::WriteStartCJumpPop(bool jumpOn){
	size_t offset = curr->write-curr->start;
	assert(offset<INT32_MAX);
	if (jumpOn){
		WriteOpCode(SLANG_OP_CNJUMP_POP);
	} else {
		WriteOpCode(SLANG_OP_CJUMP_POP);
	}
	// 4 NOOPs
	WriteSInt32(0);
	return offset;
}

inline void CodeWriter::FinishCJump(size_t offset){
	int32_t dist = curr->write-curr->start-offset;
	uint8_t* start = curr->start+offset;
	start++;
	*start++ = dist&0xFF;
	dist >>= 8;
	*start++ = dist&0xFF;
	dist >>= 8;
	*start++ = dist&0xFF;
	dist >>= 8;
	*start++ = dist&0xFF;
}

inline size_t CodeWriter::StartTryOp(){
	size_t offset = curr->write-curr->start;
	assert(offset<INT32_MAX);
	WriteOpCode(SLANG_OP_TRY);
	// 4 NOOPs
	WriteSInt32(0);
	return offset;
}

inline void CodeWriter::FinishTryOp(size_t offset){
	int32_t dist = (curr->write-curr->start+SlangOpSizes[SLANG_OP_MAYBE_WRAP])-offset;
	uint8_t* start = curr->start+offset;
	++start;
	
	*start++ = dist&0xFF;
	dist >>= 8;
	*start++ = dist&0xFF;
	dist >>= 8;
	*start++ = dist&0xFF;
	dist >>= 8;
	*start++ = dist&0xFF;
	
	WriteOpCode(SLANG_OP_MAYBE_WRAP);
}

inline bool IsDotted(SlangList* argIt){
	while (argIt){
		if (!IsList(argIt->right))
			return true;
		argIt = (SlangList*)argIt->right;
	}
	return false;
}

inline bool CodeWriter::IsPure(const SlangHeader* expr) const {
	if (GetType(expr)!=SlangType::List)
		return true;
	
	SlangList* list = (SlangList*)expr;
	SlangHeader* head = list->left;
	SlangType headType = GetType(head);
	if (headType==SlangType::List){
		if (!IsPure(head))
			return false;
	} else if (headType!=SlangType::Symbol) 
		return true;
	SymbolName sym = ((SlangObj*)head)->symbol;
	if (sym<GLOBAL_SYMBOL_COUNT){
		uint8_t purityLevel = gGlobalPurityArray[sym];
		if (purityLevel==SLANG_PURE) return true;
		if (purityLevel==SLANG_IMPURE) return false;
		
		SlangList* argIt = (SlangList*)list->right;
		if (!IsList((SlangHeader*)argIt)) return false;
		while (argIt){
			if (!IsPure(argIt->left))
				return false;
			
			argIt = (SlangList*)argIt->right;
			if (!IsList((SlangHeader*)argIt)) return false;
		}
		return true;
	}
	
	if (!knownLambdaStack.back().contains(sym))
		return false;
		
	size_t funcIndex = knownLambdaStack.back().at(sym);
	if (!lambdaCodes[funcIndex].isPure)
		return false;
	
	SlangList* argIt = (SlangList*)list->right;
	if (!IsList((SlangHeader*)argIt)) return false;
	while (argIt){
		if (!IsPure(argIt->left))
			return false;
		
		argIt = (SlangList*)argIt->right;
		if (!IsList((SlangHeader*)argIt)) return false;
	}
	
	return true;
}

bool CodeWriter::SymbolInStarParams(SymbolName sym,uint16_t& into) const {
	if (lambdaStack.empty()) return false;
	const LambdaData& lam = lambdaStack.back();
	if (!lam.isStar)
		return false;
	for (size_t i=0;i<lam.params.size;++i){
		SymbolName s = lam.params.data[i];
		if (sym==s){
			into = i;
			return true;
		}
	}
	return false;
}

bool CodeWriter::SymbolInParams(SymbolName sym,uint16_t& into) const {
	if (lambdaStack.empty()) return false;
	size_t index = lambdaStack.size()-1;
	const LambdaData* lam = &lambdaStack[index];
	while (lam->isStar){
		if (index==0) return false;
		lam = &lambdaStack[--index];
	}
	
	for (size_t i=0;i<lam->params.size;++i){
		SymbolName s = lam->params.data[i];
		if (sym==s){
			into = i;
			return true;
		}
	}
	return false;
}

bool CodeWriter::SymbolNotInAnyParams(SymbolName sym) const {
	if (lambdaStack.empty()) return true;
	size_t stackIdx = lambdaStack.size()-1;
	while (true){
		const LambdaData& lam = lambdaStack[stackIdx];
		for (size_t i=0;i<lam.params.size;++i){
			SymbolName s = lam.params.data[i];
			if (sym==s){
				return false;
			}
		}
		if (stackIdx==0)
			break;
		--stackIdx;
	}
	return true;
}

bool CodeWriter::ParamIsInited(uint16_t idx) const {
	if (lambdaStack.empty()) return true;
	const LambdaData& lam = lambdaStack.back();
	if (!lam.isLetRec) return true;
	return idx<lam.currLetInit;
}

void CodeWriter::AddClosureParam(SymbolName sym){
	if (lambdaStack.empty()) return;
	size_t stackIdx = lambdaStack.size()-1;
	while (true){
		LambdaData& lam = lambdaStack[stackIdx];
		for (size_t i=0;i<lam.params.size;++i){
			auto s = lam.params.data[i];
			if (sym==s){
				lam.isClosure = true;
				return;
			}
		}
		if (stackIdx==0)
			break;
		--stackIdx;
	}
	assert(false);
}

SlangHeader* CodeWriter::MakeIntConst(int64_t i){
	return (SlangHeader*)alloc.MakeInt(i);
}

SlangHeader* CodeWriter::MakeSymbolConst(SymbolName sym){
	return (SlangHeader*)alloc.MakeSymbol(sym);
}

bool CodeWriter::CompileData(const SlangHeader* obj){
	if (evalFuncIndex==-1ULL){
		evalFuncIndex = lambdaCodes.size();
		curr = &lambdaCodes[AllocNewBlock(32)];
	} else {
		curr = &lambdaCodes[evalFuncIndex];
		curr->write = curr->start;
		curr->isPure = true;
	}
	curr->moduleIndex = currModuleIndex;
	
	if (!CompileExpr(obj))
		return false;
		
	WriteOpCode(SLANG_OP_HALT);
	
	return true;
}

bool CodeWriter::CompileCode(const std::string& code){
	double start = GetDoublePerfTime();
	currModuleIndex = 0;
	curr = &lambdaCodes[0];
	curr->write = curr->start;
	curr->isPure = false;
	curr->moduleIndex = 0;
	
	parser.SetCodeString(code);
	SlangHeader* expr;
	while (parser.ParseLine(&expr)){
		if (!CompileExpr(expr))
			return false;
		if (parser.token.type!=SlangTokenType::EndOfFile){
			WriteOpCode(SLANG_OP_POP_ARG);
		} else {
			break;
		}
	}
	
	if (!parser.errors.empty())
		return false;
	
	WriteOpCode(SLANG_OP_HALT);
	gCompileTime = GetDoublePerfTime()-start;
	return true;
}

bool CodeWriter::CompileModule(ModuleName name,const std::string& code,size_t& funcIndex){
	currModuleIndex = name;
	parser.currModule = name;
	funcIndex = AllocNewBlock(256);
	curr = &lambdaCodes[funcIndex];
	knownLambdaStack.clear();
	knownLambdaStack.emplace_back();
	curr->write = curr->start;
	curr->isPure = true;
	
	parser.SetCodeString(code);
	SlangHeader* expr;
	while (parser.ParseLine(&expr)){
		if (!CompileExpr(expr))
			return false;
		if (parser.token.type!=SlangTokenType::EndOfFile){
			WriteOpCode(SLANG_OP_POP_ARG);
		} else {
			break;
		}
	}
	
	if (!parser.errors.empty())
		return false;
	
	WriteOpCode(SLANG_OP_HALT);
	return true;
}

bool CodeWriter::CompileDefine(const SlangHeader* head){
	SymbolName defType = ((SlangObj*)((SlangList*)head)->left)->symbol;
	// no defines in functions, use set!
	if (defType==SLANG_DEFINE&&!lambdaStack.empty()){
		DefError(head);
		return false;
	}

	SlangHeader* args = ((SlangList*)head)->right;
	
	SlangList* argIt = (SlangList*)args;
	SymbolName sym;
	if (GetType(argIt->left)!=SlangType::Symbol){
		TypeError(argIt->left,GetType(argIt->left),SlangType::Symbol);
		return false;
	}
	sym = ((SlangObj*)argIt->left)->symbol;
	if (sym<GLOBAL_SYMBOL_COUNT){
		ReservedError(head,sym);
		return false;
	}
	
	argIt = (SlangList*)argIt->right;
	
	if (defType==SLANG_DEFINE)
		currDefName = sym;
	else
		currSetName = sym;
		
	if (!CompileExpr(argIt->left))
		return false;
		
	if (defType==SLANG_DEFINE)
		currDefName = EMPTY_NAME;
	else
		currSetName = EMPTY_NAME;
	
	if (defType==SLANG_SET){
		uint16_t p;
		if (SymbolInStarParams(sym,p)){
			WriteOpCode(SLANG_OP_SET_REC);
			WriteInt16(p);
		} else if (SymbolInParams(sym,p)){
			WriteOpCode(SLANG_OP_SET_LOCAL);
			WriteInt16(p);
		} else if (SymbolNotInAnyParams(sym)){
			WriteOpCode(SLANG_OP_SET_GLOBAL);
			WriteSymbolName(sym);
		} else {
			// closure set
			WriteOpCode(SLANG_OP_SET);
			WriteSymbolName(sym);
			AddClosureParam(sym);
		}
	} else {
		WriteOpCode(SLANG_OP_DEF_GLOBAL);
		WriteSymbolName(sym);
	}
	
	WriteOpCode(SLANG_OP_NULL);
	
	return true;
}

bool CodeWriter::CompileMap(const SlangHeader* expr){
	SlangList* argIt = (SlangList*)((SlangList*)expr)->right;
	SlangHeader* func = argIt->left;
	if (GetType(func)!=SlangType::Symbol&&GetType(func)!=SlangType::List){
		TypeError(expr,GetType(func),SlangType::Lambda);
		return false;
	}
	
	WriteOpCode(SLANG_OP_PUSH_FRAME);
	
	if (func->type==SlangType::Symbol){
		SymbolName sym = ((SlangObj*)func)->symbol;
		
		if (sym<GLOBAL_SYMBOL_COUNT){
			if (!CompileConst(func))
				return false;
			
			argIt = (SlangList*)argIt->right;
			size_t argCount = 0;
			while (argIt){
				if (!CompileExpr(argIt->left))
					return false;
				argIt = (SlangList*)argIt->right;
				++argCount;
			}
			
			if (!GoodArity(sym,argCount)){
				ArityError(func,argCount,gGlobalArityArray[sym].min,gGlobalArityArray[sym].max);
				return false;
			}
			
			WriteOpCode(SLANG_OP_CALLSYM);
			WriteInt16(SLANG_MAP);
			return true;
		}
	}
		
	argIt = (SlangList*)argIt->right;
	size_t argCount = 0;
	while (argIt){
		if (!CompileExpr(argIt->left))
			return false;
		argIt = (SlangList*)argIt->right;
		++argCount;
	}
	
		
	if (!CompileExpr(func))
		return false;
	
	// where return list will be stored
	WriteOpCode(SLANG_OP_NULL);
	WriteOpCode(SLANG_OP_NULL);
	
	WriteOpCode(SLANG_OP_MAP_STEP);
	WriteInt16(argCount);
	
	return true;
}

bool CodeWriter::CompileTry(const SlangHeader* expr){
	SlangList* argIt = (SlangList*)((SlangList*)expr)->right;
	if (!argIt){
		WriteOpCode(SLANG_OP_MAYBE_NULL);
		return true;
	}
	size_t tryOffset = StartTryOp();
	
	if (!CompileExpr(argIt->left))
		return false;
	
	FinishTryOp(tryOffset);
	
	return true;
}

bool CodeWriter::CompileUnwrap(const SlangHeader* expr){
	SlangList* argIt = (SlangList*)((SlangList*)expr)->right;
	
	if (!CompileExpr(argIt->left))
		return false;
	
	WriteOpCode(SLANG_OP_MAYBE_UNWRAP);
	
	return true;
}

bool CodeWriter::CompileLambdaBody(
		const SlangHeader* head,
		const std::vector<SlangHeader*>* initExprs,
		size_t& lambdaIndex){
	size_t savedCurrOffset = curr-&lambdaCodes.front();
	uint8_t* savedLastInst = lastInst;
	SymbolName funcName = lambdaStack.back().funcName;
	lambdaIndex = lambdaCodes.size();
	
	if (funcName!=LET_SELF_SYM&&funcName!=EMPTY_NAME){
		// second def with this funcName
		if (knownLambdaStack.back().contains(funcName)){
			FuncRedefinedError(head,funcName);
			return false;
		}
		knownLambdaStack.back()[funcName] = lambdaIndex;
	} else if (funcName==EMPTY_NAME&&currSetName!=EMPTY_NAME){
		// trying to set! a known lambda
		if (knownLambdaStack.back().contains(currSetName)){
			FuncRedefinedError(head,currSetName);
			return false;
		}
	}
	
	curr = &lambdaCodes[AllocNewBlock(32)];
	curr->params = lambdaStack.back().params;
	curr->isVariadic = lambdaStack.back().isVariadic;
	curr->name = funcName;
	
	knownLambdaStack.emplace_back();
	
	if (initExprs){
		size_t oldDefName = currDefName;
		size_t argIndex = 0;
		for (const auto* expr : *initExprs){
			SymbolName param = lambdaStack.back().params.data[argIndex];
			lambdaStack.back().currLetInit = argIndex;
			if (!IsPure(expr))
				curr->isPure = false;
			currDefName = param;
			if (!CompileExpr(expr))
				return false;
			currDefName = EMPTY_NAME;
			WriteOpCode(SLANG_OP_SET_LOCAL);
			WriteInt16(argIndex++);
		}
		currDefName = oldDefName;
		lambdaStack.back().currLetInit = argIndex;
	}
	
	if (!IsPure(head))
		curr->isPure = false;
	if (!CompileExpr(head,true)){
		return false;
	}
	
	curr->isClosure = lambdaStack.back().isClosure;
	
	curr = &lambdaCodes[savedCurrOffset];
	lastInst = savedLastInst;
	knownLambdaStack.pop_back();
	return true;
}

bool CodeWriter::CompileLambda(const SlangHeader* head){
	SlangHeader* args = ((SlangList*)head)->right;
	if (args->type!=SlangType::List){
		TypeError(args,args->type,SlangType::List);
		return false;
	}
	SlangList* argIt = (SlangList*)args;
	
	if (!IsList(argIt->left)&&GetType(argIt->left)!=SlangType::Symbol){
		TypeError(argIt->left,GetType(argIt->left),SlangType::Params);
		return false;
	}
	
	bool variadic = GetType(argIt->left)==SlangType::Symbol;
	
	ParamData params = {};
	params.Reserve(4);
	std::set<SymbolName> seen = {};
	SlangList* paramIt = (SlangList*)argIt->left;
	SlangObj* symObj;
	while (paramIt){
		if (paramIt->header.type==SlangType::Symbol){
			// last variadic arg
			variadic = true;
			symObj = (SlangObj*)paramIt;
		} else if (paramIt->header.type==SlangType::List){
			if (GetType(paramIt->left)!=SlangType::Symbol){
				TypeError(paramIt->left,GetType(paramIt->left),SlangType::Symbol);
				return false;
			}
			symObj = (SlangObj*)paramIt->left;
		} else {
			TypeError((SlangHeader*)paramIt,paramIt->header.type,SlangType::List);
			return false;
		}
		
		if (seen.contains(symObj->symbol)){
			RedefinedError(head,symObj->symbol);
			return false;
		}
		
		params.PushBack(symObj->symbol);
		seen.insert(symObj->symbol);
		
		if (variadic) break;
		paramIt = (SlangList*)paramIt->right;
	}
	argIt = (SlangList*)argIt->right;

	uint64_t lambdaIndex;
	{
		LambdaData dat{currDefName,params};
		dat.isVariadic = variadic;
		lambdaStack.push_back(dat);
	}
	currDefName = EMPTY_NAME;
	if (!CompileLambdaBody(argIt->left,nullptr,lambdaIndex))
		return false;
	lambdaStack.pop_back();
	WriteOpCode(SLANG_OP_PUSH_LAMBDA);
	WriteInt64(lambdaIndex);
	return true;
}

bool CodeWriter::CompileDo(const SlangHeader* head,bool terminating){
	SlangList* argIt = (SlangList*)((SlangList*)head)->right;
	if (!argIt){
		WriteOpCode(SLANG_OP_NULL);
		if (terminating)
			WriteOpCode(SLANG_OP_RET);
		return true;
	}
	
	while (argIt->right){
		if (optimize&&!lambdaStack.empty()&&IsPure(argIt->left)){
			argIt = (SlangList*)argIt->right;
			continue;
		}
		
		if (!CompileExpr(argIt->left))
			return false;
		WriteOpCode(SLANG_OP_POP_ARG);
		
		argIt = (SlangList*)argIt->right;
	}
	
	return CompileExpr(argIt->left,terminating);
}

bool CodeWriter::CompileIf(const SlangHeader* head,bool terminating){
	SlangHeader* argIt = ((SlangList*)head)->right;
	SlangHeader* cond = ((SlangList*)argIt)->left;
	if (!CompileExpr(cond))
		return false;
	
	size_t start = WriteStartCJumpPop(false);
	
	argIt = ((SlangList*)argIt)->right;
	SlangHeader* truePath = ((SlangList*)argIt)->left;
	if (!CompileExpr(truePath,terminating))
		return false;
	
	argIt = ((SlangList*)argIt)->right;
	if (!argIt){
		if (terminating){
			FinishCJump(start);
			WriteOpCode(SLANG_OP_NULL);
			WriteOpCode(SLANG_OP_RET);
			return true;
		} else {
			size_t falseJump = WriteStartJump();
			FinishCJump(start);
			WriteOpCode(SLANG_OP_NULL);
			FinishJump(falseJump);
			return true;
		}
	}
	
	if (terminating){
		FinishCJump(start);
		SlangHeader* falsePath = ((SlangList*)argIt)->left;
		if (!CompileExpr(falsePath,terminating))
			return false;
	} else {
		size_t falseJump = WriteStartJump();
		FinishCJump(start);
		SlangHeader* falsePath = ((SlangList*)argIt)->left;
		if (!CompileExpr(falsePath,terminating))
			return false;
		FinishJump(falseJump);
	}
	
	return true;
}

bool ObjIsFalse(const SlangHeader* expr){
	if (GetType(expr)!=SlangType::Bool)
		return false;
	return expr->boolVal==false;
}

bool ObjIsElseOrTrue(const SlangHeader* expr){
	if (GetType(expr)==SlangType::Bool){
		if (expr->boolVal)
			return true;
	} else if (GetType(expr)==SlangType::Symbol){
		if (((SlangObj*)expr)->symbol==SLANG_ELSE)
			return true;
	}
	return false;
}

void CodeWriter::CaseDictAlloc(CaseDict& cd,size_t elems){
	size_t q = QuantizeToPowerOfTwo(elems*2+1);
	assert(q>=elems*2+1);
	cd.capacity = q;
	cd.elemsStart = caseDictElements.size;
	caseDictElements.AddSize(q);
	memset(caseDictElements.data+cd.elemsStart,0xFF,sizeof(CaseDictElement)*q);
}

bool CodeWriter::CaseDictSet(const CaseDict& dict,const SlangHeader* key,size_t offset){
	uint64_t hashVal = SlangHashObj(key) & (dict.capacity-1);
	
	CaseDictElement* elem = &caseDictElements.data[dict.elemsStart+hashVal];
	CaseDictElement* end = &caseDictElements.data[dict.elemsStart+dict.capacity];
	while ((uint64_t)elem->key!=DICT_UNOCCUPIED_VAL){
		if (EqualObjs(key,elem->key)){
			return false;
		}
		++elem;
		if (elem==end)
			elem = &caseDictElements.data[dict.elemsStart];
	}
	
	elem->key = key;
	elem->offset = offset;
	return true;
}

inline size_t CodeWriter::CaseDictGet(const CaseDict& dict,const SlangHeader* key) const {
	uint64_t hashVal = SlangHashObj(key) & (dict.capacity-1);
	
	CaseDictElement* elem = &caseDictElements.data[dict.elemsStart+hashVal];
	CaseDictElement* end = &caseDictElements.data[dict.elemsStart+dict.capacity];
	if ((uint64_t)elem->key==DICT_UNOCCUPIED_VAL){
		return dict.elseOffset;
	}
	
	while (!EqualObjs(key,elem->key)){
		++gCaseMisses;
		++elem;
		if (elem==end)
			elem = &caseDictElements.data[dict.elemsStart];
		if ((uint64_t)elem->key==DICT_UNOCCUPIED_VAL){
			return dict.elseOffset;
		}
	}
	return elem->offset;
}

bool CodeWriter::CompileCase(const SlangHeader* expr,bool terminating){
	SlangList* argIt = (SlangList*)((SlangList*)expr)->right;
	
	SlangHeader* val = argIt->left;
	if (!CompileExpr(val))
		return false;
	
	argIt = (SlangList*)argIt->right;
	
	size_t caseDictIndex = caseDicts.size;
	CaseDict& caseDict = caseDicts.PlaceBack();
	size_t caseJumpStart = curr->write-curr->start;
	WriteOpCode(SLANG_OP_CASE_JUMP);
	WriteInt32(caseDictIndex);
	
	Vector<CaseDictElement> cases;
	Vector<size_t> jumpToEnds;
	
	cases.Reserve(12);
	jumpToEnds.Reserve(8);
	bool elseUsed = false;
	while (argIt){
		if (GetType(argIt->left)!=SlangType::List){
			TypeError(expr,GetType(argIt->left),SlangType::List);
			return false;
		}
		SlangList* exprIt = (SlangList*)argIt->left;
		if (!exprIt->left){
			argIt = (SlangList*)argIt->right;
			continue;
		}
		
		// first the params
		// if is an else?
		if (GetType(exprIt->left)==SlangType::Symbol &&
				((SlangObj*)exprIt->left)->symbol==SLANG_ELSE){
			elseUsed = true;
			caseDict.elseOffset = (curr->write-curr->start) - caseJumpStart;
		} else {
			if (GetType(exprIt->left)!=SlangType::List){
				TypeError(argIt->left,GetType(exprIt->left),SlangType::List);
				return false;
			}
			SlangList* paramIt = (SlangList*)exprIt->left;
			size_t jumpPos = (curr->write-curr->start) - caseJumpStart;
			while (paramIt){
				if (!IsHashable(paramIt->left)){
					std::stringstream ss{};
					ss << "Case param '";
					ss << *paramIt->left;
					ss << "' is not hashable!";
					PushError(exprIt->left,"HashError",ss.str());
					return false;
				}
				CaseDictElement& elem = cases.PlaceBack();
				elem.offset = jumpPos;
				elem.key = Copy(paramIt->left);
				paramIt = (SlangList*)paramIt->right;
				if (!IsList((SlangHeader*)paramIt)){
					DotError(argIt->left);
					return false;
				}
			}
		}
		
		// onto the exprs
		exprIt = (SlangList*)exprIt->right;
		if (!exprIt){
			WriteOpCode(SLANG_OP_NULL);
			if (terminating){
				WriteOpCode(SLANG_OP_RET);
			} else {
				jumpToEnds.PushBack(WriteStartJump());
			}
			argIt = (SlangList*)argIt->right;
			continue;
		}
		if (!IsList((SlangHeader*)exprIt)){
			DotError(argIt->left);
			return false;
		}
		
		while (exprIt->right){
			if (!CompileExpr(exprIt->left))
				return false;
				
			WriteOpCode(SLANG_OP_POP_ARG);
		
			exprIt = (SlangList*)exprIt->right;
			if (!IsList((SlangHeader*)exprIt)){
				DotError(argIt->left);
				return false;
			}
		}
		
		if (!CompileExpr(exprIt->left,terminating))
			return false;
			
		if (!terminating){
			jumpToEnds.PushBack(WriteStartJump());
		}
		
		argIt = (SlangList*)argIt->right;
	}
	
	CaseDictAlloc(caseDict,cases.size);
	
	if (!elseUsed){
		caseDict.elseOffset = (curr->write-curr->start) - caseJumpStart;
	}
	
	for (size_t i=0;i<cases.size;++i){
		if (!CaseDictSet(caseDict,cases.data[i].key,cases.data[i].offset)){
			std::stringstream ss{};
			ss << "Key '";
			if (cases.data[i].key)
				ss << *cases.data[i].key;
			else
				ss << "()";
			
			ss << "' was reused!";
			PushError(expr,"CaseError",ss.str());
			return false;
		}
	}
	
	WriteOpCode(SLANG_OP_NULL);
	if (terminating){
		WriteOpCode(SLANG_OP_RET);
	} else {
		for (size_t i=0;i<jumpToEnds.size;++i){
			FinishJump(jumpToEnds.data[i]);
		}
	}
	
	return true;
}

bool CodeWriter::CompileCond(const SlangHeader* expr,bool terminating){
	SlangList* argIt = (SlangList*)((SlangList*)expr)->right;
	
	Vector<size_t> jumpToEnds;
	jumpToEnds.Reserve(8);
	while (argIt){
		if (GetType((SlangHeader*)argIt)!=SlangType::List){
			DotError(expr);
			return false;
		}
		SlangHeader* params = argIt->left;
		if (GetType(params)!=SlangType::List){
			TypeError(params,GetType(params),SlangType::List);
			return false;
		}
		SlangList* paramIt = (SlangList*)params;
		SlangHeader* cond = paramIt->left;
		
		if (ObjIsFalse(cond)){
			argIt = (SlangList*)argIt->right;
			continue;
		}
		
		if (ObjIsElseOrTrue(cond)){
			paramIt = (SlangList*)paramIt->right;
			if (GetType((SlangHeader*)paramIt)!=SlangType::List){
				TypeError(params,GetType((SlangHeader*)paramIt),SlangType::List);
				return false;
			}
			while (paramIt->right){
				if (optimize&&IsPure(paramIt->left)){
                    paramIt = (SlangList*)paramIt->right;
					if (GetType((SlangHeader*)paramIt)!=SlangType::List){
						DotError(params);
						return false;
					}
					continue;
				}
				
				if (!CompileExpr(paramIt->left))
					return false;
				WriteOpCode(SLANG_OP_POP_ARG);
				
                paramIt = (SlangList*)paramIt->right;
				if (GetType((SlangHeader*)paramIt)!=SlangType::List){
					DotError(params);
					return false;
				}
			}
			
			if (!CompileExpr(paramIt->left,terminating))
				return false;
				
			if (!terminating){
				for (size_t i=0;i<jumpToEnds.size;++i){
					FinishJump(jumpToEnds.data[i]);
				}
			}
			
			return true;
		}
		
		if (!CompileExpr(cond))
			return false;
		size_t jumpOver = WriteStartCJumpPop(false);
		paramIt = (SlangList*)paramIt->right;
		if (GetType((SlangHeader*)paramIt)!=SlangType::List){
			DotError(params);
			return false;
		}
		while (paramIt->right){
			if (optimize&&IsPure(paramIt->left)){
                paramIt = (SlangList*)paramIt->right;
				if (GetType((SlangHeader*)paramIt)!=SlangType::List){
					DotError(params);
					return false;
				}
				continue;
			}
		
			if (!CompileExpr(paramIt->left))
				return false;
			WriteOpCode(SLANG_OP_POP_ARG);
			
            paramIt = (SlangList*)paramIt->right;
			if (GetType((SlangHeader*)paramIt)!=SlangType::List){
				DotError(params);
				return false;
			}
		}
		
		if (!CompileExpr(paramIt->left,terminating))
			return false;
		
		if (!terminating){
			jumpToEnds.PushBack(WriteStartJump());
		}
		FinishCJump(jumpOver);
		argIt = (SlangList*)argIt->right;
	}
	WriteOpCode(SLANG_OP_NULL);
	if (terminating){
		WriteOpCode(SLANG_OP_RET);
	} else {
		for (size_t i=0;i<jumpToEnds.size;++i){
			FinishJump(jumpToEnds.data[i]);
		}
	}
	return true;
}

bool CodeWriter::CompileLetRec(const SlangHeader* expr,bool terminating){
	SymbolName letName = LET_SELF_SYM;
	SlangHeader* argIt = ((SlangList*)expr)->right;
	SlangHeader* params = ((SlangList*)argIt)->left;
	if (!IsList(params)){
		TypeError(params,GetType(params),SlangType::List);
		return false;
	}
	
	SlangList* paramIt = (SlangList*)params;
	
	ParamData p{};
	p.Reserve(4);
	std::vector<SlangHeader*> initExprs{};
	initExprs.reserve(4);
	{
		LambdaData dat{letName,p};
		dat.isLetRec = true;
		lambdaStack.push_back(dat);
	}
	std::set<SymbolName> seen{};
	
	WriteOpCode(SLANG_OP_PUSH_FRAME);
	while (paramIt){
		SlangHeader* pairH = paramIt->left;
		if (!IsList(pairH)){
			TypeError(pairH,GetType(pairH),SlangType::List);
			return false;
		}
		SlangList* pair = (SlangList*)pairH;
		size_t pairLen = GetArgCount(pair);
		ARITY_CHECK_EXACT(pairH,pairLen,2);
		SlangHeader* symH = pair->left;
		if (GetType(symH)!=SlangType::Symbol){
			TypeError(symH,GetType(symH),SlangType::Symbol);
			return false;
		}
		SymbolName sym = ((SlangObj*)symH)->symbol;
		
		if (seen.contains(sym)||letName==sym){
			RedefinedError(expr,sym);
			return false;
		}
		
		pair = (SlangList*)pair->right;
		SlangHeader* val = pair->left;
		initExprs.push_back(val);
		WriteOpCode(SLANG_OP_NULL);
		
		lambdaStack.back().params.PushBack(sym);
		seen.insert(sym);
		
		paramIt = (SlangList*)paramIt->right;
		if (!IsList((SlangHeader*)paramIt)){
			TypeError((SlangHeader*)paramIt,GetType((SlangHeader*)paramIt),SlangType::List);
			return false;
		}
	}
	
	argIt = ((SlangList*)argIt)->right;
	SlangHeader* body = ((SlangList*)argIt)->left;
	size_t lamIndex;
	if (!CompileLambdaBody(body,&initExprs,lamIndex))
		return false;
	lambdaStack.pop_back();
	WriteOpCode(SLANG_OP_PUSH_LAMBDA);
	WriteInt64(lamIndex);
	
	if (terminating){
		WriteOpCode(SLANG_OP_RETCALL);
	} else {
		WriteOpCode(SLANG_OP_CALL);
	}
	return true;
}

bool CodeWriter::CompileLet(const SlangHeader* expr,bool terminating){
	size_t argCount = GetArgCount((SlangList*)((SlangList*)expr)->right);
	
	SymbolName letName = LET_SELF_SYM;
	SlangHeader* argIt = ((SlangList*)expr)->right;
	if (argCount==3){
		SlangHeader* nameH = ((SlangList*)argIt)->left;
		if (GetType(nameH)!=SlangType::Symbol){
			TypeError(nameH,GetType(nameH),SlangType::Symbol);
			return false;
		}
		letName = ((SlangObj*)nameH)->symbol;
		argIt = ((SlangList*)argIt)->right;
	}
	
	SlangHeader* params = ((SlangList*)argIt)->left;
	if (!IsList(params)){
		TypeError(params,GetType(params),SlangType::List);
		return false;
	}
	
	SlangList* paramIt = (SlangList*)params;
	
	{
		ParamData p{};
		p.Reserve(4);
		LambdaData dat{letName,p};
		dat.isStar = true;
		lambdaStack.push_back(dat);
	}
	std::set<SymbolName> seen{};
	
	WriteOpCode(SLANG_OP_PUSH_FRAME);
	while (paramIt){
		SlangHeader* pairH = paramIt->left;
		if (!IsList(pairH)){
			TypeError(pairH,GetType(pairH),SlangType::List);
			return false;
		}
		SlangList* pair = (SlangList*)pairH;
		size_t pairLen = GetArgCount(pair);
		ARITY_CHECK_EXACT(pairH,pairLen,2);
		SlangHeader* symH = pair->left;
		if (GetType(symH)!=SlangType::Symbol){
			TypeError(symH,GetType(symH),SlangType::Symbol);
			return false;
		}
		SymbolName sym = ((SlangObj*)symH)->symbol;
		
		if (seen.contains(sym)||letName==sym){
			RedefinedError(expr,sym);
			return false;
		}
		
		pair = (SlangList*)pair->right;
		SlangHeader* val = pair->left;
		if (!CompileExpr(val))
			return false;
		
		lambdaStack.back().params.PushBack(sym);
		seen.insert(sym);
		
		paramIt = (SlangList*)paramIt->right;
		if (!IsList((SlangHeader*)paramIt)){
			TypeError((SlangHeader*)paramIt,GetType((SlangHeader*)paramIt),SlangType::List);
			return false;
		}
	}
	
	argIt = ((SlangList*)argIt)->right;
	SlangHeader* body = ((SlangList*)argIt)->left;
	size_t lamIndex;
	lambdaStack.back().isStar = false;
	knownLambdaStack.emplace_back();
	if (!CompileLambdaBody(body,nullptr,lamIndex))
		return false;
	knownLambdaStack.pop_back();
	lambdaStack.pop_back();
	WriteOpCode(SLANG_OP_PUSH_LAMBDA);
	WriteInt64(lamIndex);
	
	if (terminating){
		WriteOpCode(SLANG_OP_RETCALL);
	} else {
		WriteOpCode(SLANG_OP_CALL);
	}
	return true;
}

bool CodeWriter::CompileApply(const SlangHeader* expr,bool terminating){
	size_t argCount = GetArgCount((SlangList*)((SlangList*)expr)->right);
	
	SlangHeader* argIt = ((SlangList*)expr)->right;
	SlangHeader* func = ((SlangList*)argIt)->left;
	
	argIt = ((SlangList*)argIt)->right;
	
	WriteOpCode(SLANG_OP_PUSH_FRAME);
	
	for (size_t i=0;i<argCount-2;++i){
		SlangHeader* v = ((SlangList*)argIt)->left;
		if (!CompileExpr(v))
			return false;
		
		argIt = ((SlangList*)argIt)->right;
	}
	
	SlangHeader* args = ((SlangList*)argIt)->left;
	if (args!=nullptr){
		if (!CompileExpr(args))
			return false;
		
		WriteOpCode(SLANG_OP_UNPACK);
	}
	
	if (!CompileExpr(func))
		return false;
	
	if (terminating){
		WriteOpCode(SLANG_OP_RETCALL);
	} else {
		WriteOpCode(SLANG_OP_CALL);
	}
	return true;
}

bool CodeWriter::CompileAnd(const SlangHeader* expr,bool terminating){
	SlangList* argIt = (SlangList*)((SlangList*)expr)->right;
	if (!argIt){
		WriteOpCode(SLANG_OP_BOOL_TRUE);
		if (terminating)
			WriteOpCode(SLANG_OP_RET);
		return true;
	}
	Vector<size_t> jumpStarts;
	jumpStarts.Reserve(8);
	
	while (argIt->right){
		if (!CompileExpr(argIt->left))
			return false;
		
		jumpStarts.PushBack(WriteStartCJump(false));
		WriteOpCode(SLANG_OP_POP_ARG);
		
		argIt = (SlangList*)argIt->right;
	}
	
	if (!CompileExpr(argIt->left))
		return false;
	
	for (size_t i=0;i<jumpStarts.size;++i){
		FinishCJump(jumpStarts.data[i]);
	}
	
	if (terminating){
		WriteOpCode(SLANG_OP_RET);
	}
	
	return true;
}

bool CodeWriter::CompileOr(const SlangHeader* expr,bool terminating){
	SlangList* argIt = (SlangList*)((SlangList*)expr)->right;
	if (!argIt){
		WriteOpCode(SLANG_OP_BOOL_FALSE);
		if (terminating)
			WriteOpCode(SLANG_OP_RET);
		return true;
	}
	Vector<size_t> jumpStarts;
	jumpStarts.Reserve(8);
	
	while (argIt->right){
		if (!CompileExpr(argIt->left))
			return false;
		
		jumpStarts.PushBack(WriteStartCJump(true));
		WriteOpCode(SLANG_OP_POP_ARG);
		
		argIt = (SlangList*)argIt->right;
	}
	if (!CompileExpr(argIt->left))
		return false;
	
	for (size_t i=0;i<jumpStarts.size;++i){
		FinishCJump(jumpStarts.data[i]);
	}
	
	if (terminating){
		WriteOpCode(SLANG_OP_RET);
	}
	
	return true;
}

bool CodeWriter::CompileConst(const SlangHeader* obj){
	SlangObj* asObj = (SlangObj*)obj;
	SymbolName sym;
	switch (GetType(obj)){
		case SlangType::NullType:
			WriteOpCode(SLANG_OP_NULL);
			break;
		case SlangType::Int:
			if (asObj->integer==0){
				WriteOpCode(SLANG_OP_ZERO);
			} else if (asObj->integer==1){
				WriteOpCode(SLANG_OP_ONE);
			} else {
				WriteOpCode(SLANG_OP_LOAD_PTR);
				WritePointer(Copy(obj));
			}
			break;
		case SlangType::Real:
			WriteOpCode(SLANG_OP_LOAD_PTR);
			WritePointer(Copy(obj));
			break;
		case SlangType::Bool:
			if (obj->boolVal)
				WriteOpCode(SLANG_OP_BOOL_TRUE);
			else
				WriteOpCode(SLANG_OP_BOOL_FALSE);
			break;
		case SlangType::String:
			WriteOpCode(SLANG_OP_LOAD_PTR);
			WritePointer(Copy(obj));
			break;
		case SlangType::Vector:
			WriteOpCode(SLANG_OP_LOAD_PTR);
			WritePointer(Copy(obj));
			break;
		case SlangType::Symbol:
			sym = asObj->symbol;
			uint16_t idx;
			if (SymbolInStarParams(sym,idx)){
				// let local var
				WriteOpCode(SLANG_OP_GET_REC);
				WriteInt16(idx);
			} else if (SymbolInParams(sym,idx)){
				// local var
				if (!ParamIsInited(idx)){
					LetRecError(obj,sym);
					return false;
				}
				WriteOpCode(SLANG_OP_GET_LOCAL);
				WriteInt16(idx);
			} else if (SymbolNotInAnyParams(sym)){
				// global var
				if (sym>=GLOBAL_SYMBOL_COUNT){
					WriteOpCode(SLANG_OP_GET_GLOBAL);
					WriteSymbolName(sym);
					AddCodeLocation(obj);
				} else {
					WriteOpCode(SLANG_OP_LOAD_PTR);
					WritePointer(Copy(obj));
				}
			} else {
				// closure var
				WriteOpCode(SLANG_OP_LOOKUP);
				WriteSymbolName(sym);
				AddClosureParam(sym);
				AddCodeLocation(obj);
			}
			break;
		default:
			return false;
	}
	return true;
}

bool CodeWriter::CompileQuote(const SlangHeader* expr){
	SlangHeader* argIt = ((SlangList*)expr)->right;
	SlangHeader* qObj = ((SlangList*)argIt)->left;
	SlangType qType = GetType(qObj);
	
	switch (qType){
		case SlangType::NullType:
		case SlangType::Int:
		case SlangType::Real:
		case SlangType::Bool:
		case SlangType::EndOfFile:
			return CompileConst(qObj);
		default:
			break;
	}
	
	WriteOpCode(SLANG_OP_LOAD_PTR);
	WritePointer(Copy(qObj));
		
	return true;
}

bool CodeWriter::CompileUnquote(const SlangHeader* expr){
	SlangList* argIt = (SlangList*)((SlangList*)expr)->right;
	if (argIt->right){
		ArityError(expr,GetArgCount((SlangList*)expr)-1,1,1);
		return false;
	}
	if (!CompileExpr(argIt->left))
		return false;
	
	return true;
}

bool CodeWriter::CompileUnquoteSplicing(const SlangHeader* expr){
	SlangList* argIt = (SlangList*)((SlangList*)expr)->right;
	if (argIt->right){
		ArityError(expr,GetArgCount((SlangList*)expr)-1,1,1);
		return false;
	}
	if (!CompileExpr(argIt->left))
		return false;
	
	return true;
}

inline bool IsUnquoteProc(const SlangHeader* expr){
	if (GetType(expr)==SlangType::List){
		SlangHeader* l = ((SlangList*)expr)->left;
		if (GetType(l)==SlangType::Symbol){
			SymbolName s = ((SlangObj*)l)->symbol;
			return s==SLANG_UNQUOTE;
		}
	}
	return false;
}

inline bool IsUnquoteSplicingProc(const SlangHeader* expr){
	if (GetType(expr)==SlangType::List){
		SlangHeader* l = ((SlangList*)expr)->left;
		if (GetType(l)==SlangType::Symbol){
			SymbolName s = ((SlangObj*)l)->symbol;
			return s==SLANG_UNQUOTE_SPLICING;
		}
	}
	return false;
}

inline bool IsQuasiquoteProc(const SlangHeader* expr){
	if (GetType(expr)==SlangType::List){
		SlangHeader* l = ((SlangList*)expr)->left;
		if (GetType(l)==SlangType::Symbol){
			SymbolName s = ((SlangObj*)l)->symbol;
			return s==SLANG_QUASIQUOTE;
		}
	}
	return false;
}

bool CodeWriter::QuasiquoteVector(const SlangVec* vec,size_t depth){
	if (!vec->storage){
		WriteOpCode(SLANG_OP_LOAD_PTR);
		WritePointer(Copy((SlangHeader*)vec));
		WriteOpCode(SLANG_OP_COPY);
		return true;
	}
	WriteOpCode(SLANG_OP_PUSH_FRAME);
	for (size_t i=0;i<vec->storage->size;++i){
		SlangHeader* obj = vec->storage->objs[i];
		SlangType oType = GetType(obj);
		switch (oType){
			case SlangType::List:
				if (IsUnquoteProc(obj)){
					if (depth==0){
						if (!CompileUnquote(obj))
							return false;
					} else {
						if (!QuasiquoteList((SlangList*)obj,depth-1))
							return false;
					}
				} else if (IsUnquoteSplicingProc(obj)){
					if (depth==0){
						if (!CompileUnquoteSplicing(obj))
							return false;
						WriteOpCode(SLANG_OP_UNPACK);
					} else {
						if (!QuasiquoteList((SlangList*)obj,depth-1))
							return false;
					}
				} else if (IsQuasiquoteProc(obj)){
					if (!QuasiquoteList((SlangList*)obj,depth+1))
						return false;
				} else {
					if (!QuasiquoteList((SlangList*)obj,depth))
						return false;
				}
				break;
			case SlangType::Vector:
				if (!QuasiquoteVector(vec,depth))
					return false;
				break;
			default:
				if (!CompileConst(obj))
					return false;
				break;
		}
	}
	
	WriteOpCode(SLANG_OP_MAKE_VEC);
	return true;
}

bool CodeWriter::QuasiquoteList(const SlangList* list,size_t depth){
	SlangType lType = GetType(list->left);
	bool spliced = false;
	switch (lType){
		case SlangType::List: {
			if (IsUnquoteProc(list->left)){
				if (depth==0){
					if (!CompileUnquote(list->left))
						return false;
				} else {
					if (!QuasiquoteList((SlangList*)list->left,depth-1))
						return false;
				}
			} else if (IsUnquoteSplicingProc(list->left)){
				if (depth==0){
					if (!CompileUnquoteSplicing(list->left))
						return false;
					spliced = true;
				} else {
					if (!QuasiquoteList((SlangList*)list->left,depth-1))
						return false;
				}
			} else {
				size_t newDepth = IsQuasiquoteProc(list->left) ? depth+1 : depth;
				if (!QuasiquoteList((SlangList*)list->left,newDepth))
					return false;
			}
			break;
		}
		case SlangType::Vector: {
			if (!QuasiquoteVector((SlangVec*)list->left,depth))
				return false;
			break;
		}
		
		default:
			if (!CompileConst(list->left))
				return false;
			break;
	}
	
	if (spliced && !list->right){
		return true;
	}
	
	SlangType rType = GetType(list->right);
	if (rType==SlangType::Vector){
		if (!QuasiquoteVector((SlangVec*)list->right,depth))
			return false;
	} else if (rType!=SlangType::List){
		if (!CompileConst(list->right))
			return false;
	} else {
		if (!QuasiquoteList((SlangList*)list->right,depth))
			return false;
	}
	
	if (spliced){
		WriteOpCode(SLANG_OP_LIST_CONCAT);
		WriteInt16(2);
	} else {
		WriteOpCode(SLANG_OP_PAIR);
	}
	return true;
}

bool CodeWriter::CompileQuasiquote(const SlangHeader* expr){
	SlangHeader* argIt = ((SlangList*)expr)->right;
	SlangHeader* qObj = ((SlangList*)argIt)->left;
	
	SlangType qType = GetType(qObj);
	if (qType==SlangType::List){
		if (IsUnquoteProc(qObj)){
			if (!CompileUnquote(qObj))
				return false;
		} else {
			size_t depth = IsQuasiquoteProc(qObj) ? 1 : 0;
			if (!QuasiquoteList((SlangList*)qObj,depth))
				return false;
		}
	} else if (qType==SlangType::Vector){
		if (!QuasiquoteVector((SlangVec*)qObj,0))
			return false;
	} else {
		if (!CompileConst(qObj))
			return false;
	}
	
	return true;
}

bool CodeWriter::CompileNot(const SlangHeader* expr){
	SlangHeader* argIt = ((SlangList*)expr)->right;
	SlangHeader* condObj = ((SlangList*)argIt)->left;
	if (!CompileExpr(condObj))
		return false;
	WriteOpCode(SLANG_OP_NOT);
	return true;
}

bool CodeWriter::CompileInc(const SlangHeader* expr){
	SlangHeader* argIt = ((SlangList*)expr)->right;
	SlangHeader* numObj = ((SlangList*)argIt)->left;
	if (!CompileExpr(numObj))
		return false;
	WriteOpCode(SLANG_OP_INC);
	return true;
}

bool CodeWriter::CompileDec(const SlangHeader* expr){
	SlangHeader* argIt = ((SlangList*)expr)->right;
	SlangHeader* numObj = ((SlangList*)argIt)->left;
	if (!CompileExpr(numObj))
		return false;
	WriteOpCode(SLANG_OP_DEC);
	return true;
}

bool CodeWriter::CompileAdd(const SlangHeader* expr){
	size_t argCount = GetArgCount((SlangList*)((SlangList*)expr)->right);
	
	if (argCount==0){
		WriteOpCode(SLANG_OP_LOAD_PTR);
		WritePointer(constOneObj);
		return true;
	}
	
	SlangHeader* argIt = ((SlangList*)expr)->right;
	if (argCount==1){
		SlangHeader* single = ((SlangList*)argIt)->left;
		return CompileExpr(single);
	}
	
	uint16_t count = 0;
	while (argIt){
		SlangHeader* obj = ((SlangList*)argIt)->left;
		if (!CompileExpr(obj))
			return false;
		
		argIt = ((SlangList*)argIt)->right;
		++count;
	}
	
	WriteOpCode(SLANG_OP_ADD);
	WriteInt16(count);
	return true;
}

bool CodeWriter::CompileSub(const SlangHeader* expr){
	size_t argCount = GetArgCount((SlangList*)((SlangList*)expr)->right);
	
	SlangHeader* argIt = ((SlangList*)expr)->right;
	if (argCount==1){
		SlangHeader* single = ((SlangList*)argIt)->left;
		if (!CompileExpr(single))
			return false;
		WriteOpCode(SLANG_OP_NEG);
		return true;
	}
	
	uint16_t count = 0;
	while (argIt){
		SlangHeader* obj = ((SlangList*)argIt)->left;
		if (!CompileExpr(obj))
			return false;
		
		argIt = ((SlangList*)argIt)->right;
		++count;
	}
	
	WriteOpCode(SLANG_OP_SUB);
	WriteInt16(count);
	return true;
}

bool CodeWriter::CompileMul(const SlangHeader* expr){
	size_t argCount = GetArgCount((SlangList*)((SlangList*)expr)->right);
	
	if (argCount==0){
		WriteOpCode(SLANG_OP_LOAD_PTR);
		WritePointer(constZeroObj);
		return true;
	}
	
	SlangHeader* argIt = ((SlangList*)expr)->right;
	if (argCount==1){
		SlangHeader* single = ((SlangList*)argIt)->left;
		return CompileExpr(single);
	}
	
	uint16_t count = 0;
	while (argIt){
		SlangHeader* obj = ((SlangList*)argIt)->left;
		if (!CompileExpr(obj))
			return false;
		
		argIt = ((SlangList*)argIt)->right;
		++count;
	}
	
	WriteOpCode(SLANG_OP_MUL);
	WriteInt16(count);
	return true;
}

bool CodeWriter::CompileDiv(const SlangHeader* expr){
	size_t argCount = GetArgCount((SlangList*)((SlangList*)expr)->right);
	
	SlangHeader* argIt = ((SlangList*)expr)->right;
	if (argCount==1){
		SlangHeader* single = ((SlangList*)argIt)->left;
		if (!CompileExpr(single))
			return false;
		WriteOpCode(SLANG_OP_INVERT);
		return true;
	}
	
	uint16_t count = 0;
	while (argIt){
		SlangHeader* obj = ((SlangList*)argIt)->left;
		if (!CompileExpr(obj))
			return false;
		
		argIt = ((SlangList*)argIt)->right;
		++count;
	}
	
	WriteOpCode(SLANG_OP_DIV);
	WriteInt16(count);
	return true;
}

bool CodeWriter::CompileEq(const SlangHeader* expr){
	SlangHeader* argIt = ((SlangList*)expr)->right;
	SlangHeader* lObj = ((SlangList*)argIt)->left;
	if (!CompileExpr(lObj))
		return false;
	argIt = ((SlangList*)argIt)->right;
	SlangHeader* rObj = ((SlangList*)argIt)->left;
	if (!CompileExpr(rObj))
		return false;
	WriteOpCode(SLANG_OP_EQ);
	return true;
}

bool CodeWriter::CompilePair(const SlangHeader* expr){
	SlangHeader* argIt = ((SlangList*)expr)->right;
	SlangHeader* lObj = ((SlangList*)argIt)->left;
	if (!CompileExpr(lObj))
		return false;
		
	argIt = ((SlangList*)argIt)->right;
	SlangHeader* rObj = ((SlangList*)argIt)->left;
	if (!CompileExpr(rObj))
		return false;
		
	WriteOpCode(SLANG_OP_PAIR);
	return true;
}

bool CodeWriter::CompileLeft(const SlangHeader* expr){
	SlangHeader* argIt = ((SlangList*)expr)->right;
	SlangHeader* obj = ((SlangList*)argIt)->left;
	if (!CompileExpr(obj))
		return false;
	WriteOpCode(SLANG_OP_LEFT);
	return true;
}

bool CodeWriter::CompileRight(const SlangHeader* expr){
	SlangHeader* argIt = ((SlangList*)expr)->right;
	SlangHeader* obj = ((SlangList*)argIt)->left;
	if (!CompileExpr(obj))
		return false;
	WriteOpCode(SLANG_OP_RIGHT);
	return true;
}

bool CodeWriter::CompileSetLeft(const SlangHeader* expr){
	SlangHeader* argIt = ((SlangList*)expr)->right;
	SlangHeader* obj = ((SlangList*)argIt)->left;
	if (!CompileExpr(obj))
		return false;
		
	argIt = ((SlangList*)argIt)->right;
	SlangHeader* val = ((SlangList*)argIt)->left;
	if (!CompileExpr(val))
		return false;
		
	WriteOpCode(SLANG_OP_SET_LEFT);
	WriteOpCode(SLANG_OP_NULL);
	return true;
}

bool CodeWriter::CompileSetRight(const SlangHeader* expr){
	SlangHeader* argIt = ((SlangList*)expr)->right;
	SlangHeader* obj = ((SlangList*)argIt)->left;
	if (!CompileExpr(obj))
		return false;
		
	argIt = ((SlangList*)argIt)->right;
	SlangHeader* val = ((SlangList*)argIt)->left;
	if (!CompileExpr(val))
		return false;
		
	WriteOpCode(SLANG_OP_SET_RIGHT);
	WriteOpCode(SLANG_OP_NULL);
	return true;
}

bool CodeWriter::CompileVec(const SlangHeader* expr){
	WriteOpCode(SLANG_OP_PUSH_FRAME);
	SlangList* argIt = (SlangList*)((SlangList*)expr)->right;
	while (argIt){
		if (!CompileExpr(argIt->left))
			return false;
		
		argIt = (SlangList*)argIt->right;
	}
	
	WriteOpCode(SLANG_OP_MAKE_VEC);
	return true;
}

bool CodeWriter::CompileIsPure(const SlangHeader* expr){
	SlangHeader* argIt = ((SlangList*)expr)->right;
	SlangHeader* obj = ((SlangList*)argIt)->left;
	if (IsPure(obj))
		WriteOpCode(SLANG_OP_BOOL_TRUE);
	else
		WriteOpCode(SLANG_OP_BOOL_FALSE);
		
	return true;
}

bool CodeWriter::CompileExport(const SlangHeader* expr){
	SlangHeader* argIt = ((SlangList*)expr)->right;
	SlangHeader* obj = ((SlangList*)argIt)->left;
	if (GetType(obj)!=SlangType::Symbol){
		TypeError(expr,GetType(obj),SlangType::Symbol);
		return false;
	}
	
	WriteOpCode(SLANG_OP_EXPORT);
	WriteSymbolName(((SlangObj*)obj)->symbol);
	WriteOpCode(SLANG_OP_NULL);
	return true;
}

bool CodeWriter::CompileImport(const SlangHeader* expr){
	if (!lambdaStack.empty()){
		PushError(expr,"ImportError","Imports must happen at the global level!");
		return false;
	}
	SlangHeader* argIt = ((SlangList*)expr)->right;
	SlangHeader* obj = ((SlangList*)argIt)->left;
	if (GetType(obj)!=SlangType::List){
		TypeError(expr,GetType(obj),SlangType::List);
		return false;
	}
	if (IsDotted((SlangList*)obj)){
		DotError(obj);
		return false;
	}
	
	SlangList* nameIt = (SlangList*)obj;
	std::string_view part;
	while (nameIt){
		SlangHeader* symObj = nameIt->left;
		if (GetType(symObj)!=SlangType::Symbol){
			TypeError(obj,GetType(symObj),SlangType::Symbol);
			return false;
		}
		part = parser.GetSymbolString(((SlangObj*)symObj)->symbol);
		if (!IsValidPathPart(part)){
			PushError(((SlangList*)argIt)->left,
				"ImportError","Invalid import symbol!");
			return false;
		}
		nameIt = (SlangList*)nameIt->right;
	}
	
	WriteOpCode(SLANG_OP_IMPORT);
	WritePointer(Copy(obj));
	WriteOpCode(SLANG_OP_NULL);
	
	return true;
}

#define COMPILE_GLOBAL_SYM(func) \
	if (!func(expr)) \
		return false; \
	if (terminating) \
		WriteOpCode(SLANG_OP_RET); \
	AddCodeLocation(expr); \
	return true

bool CodeWriter::CompileExpr(const SlangHeader* expr,bool terminating){
	if (GetType(expr)!=SlangType::List){
		if (!CompileConst(expr))
			return false;
		if (terminating){
			WriteOpCode(SLANG_OP_RET);
		}
		return true;
	}
	bool recurse = false;
	
	SlangList* list = (SlangList*)expr;
	
	SlangHeader* head = list->left;
	SlangType t = GetType(head);
	if (t!=SlangType::Symbol&&t!=SlangType::List){
		TypeError(head,t,SlangType::Lambda);
		return false;
	}
	
	if (IsDotted(list)){
		DotError((SlangHeader*)list);
		return false;
	}
	
	size_t argCount = GetArgCount(list)-1;
	bool inlineSymHead = false;
	
	if (t==SlangType::Symbol){
		SymbolName sym = ((SlangObj*)head)->symbol;
		if (sym<GLOBAL_SYMBOL_COUNT){
			if (!GoodArity(sym,argCount)){
				ArityError(head,argCount,gGlobalArityArray[sym].min,gGlobalArityArray[sym].max);
				return false;
			}
			inlineSymHead = true;
			// special cases (def, &, ...)
			switch (sym){
				case SLANG_DEFINE:
				case SLANG_SET:
					COMPILE_GLOBAL_SYM(CompileDefine);
				case SLANG_DO:
					return CompileDo(expr,terminating);
				case SLANG_IF:
					return CompileIf(expr,terminating);
				case SLANG_COND:
					return CompileCond(expr,terminating);
				case SLANG_CASE:
					return CompileCase(expr,terminating);
				case SLANG_LET:
					return CompileLet(expr,terminating);
				case SLANG_LETREC:
					return CompileLetRec(expr,terminating);
				case SLANG_APPLY:
					return CompileApply(expr,terminating);
				case SLANG_AND:
					return CompileAnd(expr,terminating);
				case SLANG_OR:
					return CompileOr(expr,terminating);
				case SLANG_MAP:
					COMPILE_GLOBAL_SYM(CompileMap);
				case SLANG_TRY:
					COMPILE_GLOBAL_SYM(CompileTry);
				case SLANG_UNWRAP:
					COMPILE_GLOBAL_SYM(CompileUnwrap);
				case SLANG_LAMBDA:
					COMPILE_GLOBAL_SYM(CompileLambda);
				case SLANG_QUOTE:
					COMPILE_GLOBAL_SYM(CompileQuote);
				case SLANG_QUASIQUOTE:
					COMPILE_GLOBAL_SYM(CompileQuasiquote);
				case SLANG_UNQUOTE:
					PushError(expr,"QuasiquoteError",
						"unquote must appear inside quasiquote!"
					);
					return false;
				case SLANG_UNQUOTE_SPLICING:
					PushError(expr,"QuasiquoteError",
						"unquote-splicing must appear inside quasiquote!"
					);
					return false;
				case SLANG_NOT:
					COMPILE_GLOBAL_SYM(CompileNot);
				case SLANG_INC:
					COMPILE_GLOBAL_SYM(CompileInc);
				case SLANG_DEC:
					COMPILE_GLOBAL_SYM(CompileDec);
				case SLANG_ADD:
					COMPILE_GLOBAL_SYM(CompileAdd);
				case SLANG_SUB:
					COMPILE_GLOBAL_SYM(CompileSub);
				case SLANG_MUL:
					COMPILE_GLOBAL_SYM(CompileMul);
				case SLANG_DIV:
					COMPILE_GLOBAL_SYM(CompileDiv);
				case SLANG_EQ:
					if (argCount==2){
						COMPILE_GLOBAL_SYM(CompileEq);
					} else if (argCount<=1){
						WriteOpCode(SLANG_OP_BOOL_TRUE);
						if (terminating)
							WriteOpCode(SLANG_OP_RET);
						return true;
					}
					break;
				case SLANG_PAIR:
					COMPILE_GLOBAL_SYM(CompilePair);
				case SLANG_LEFT:
					COMPILE_GLOBAL_SYM(CompileLeft);
				case SLANG_RIGHT:
					COMPILE_GLOBAL_SYM(CompileRight);
				case SLANG_SET_LEFT:
					COMPILE_GLOBAL_SYM(CompileSetLeft);
				case SLANG_SET_RIGHT:
					COMPILE_GLOBAL_SYM(CompileSetRight);
				case SLANG_VEC:
					COMPILE_GLOBAL_SYM(CompileVec);
				case SLANG_IS_PURE:
					COMPILE_GLOBAL_SYM(CompileIsPure);
				case SLANG_EXPORT:
					COMPILE_GLOBAL_SYM(CompileExport);
				case SLANG_IMPORT:
					COMPILE_GLOBAL_SYM(CompileImport);
			}
		} else if (!lambdaStack.empty()&&sym==lambdaStack.back().funcName){
			recurse = true;
		}
	}
	
	WriteOpCode(SLANG_OP_PUSH_FRAME);
	
	SlangList* argIt = (SlangList*)list->right;
	for (size_t i=0;i<argCount;++i){
		if (!CompileExpr(argIt->left))
			return false;
		
		argIt = (SlangList*)argIt->right;
	}
	
	if (!IsList((SlangHeader*)argIt)) return false;
	
	if (!(terminating&&recurse)){
		if (!inlineSymHead){
			if (!CompileExpr(head))
				return false;
		}
	}
	
	if (terminating&&recurse)
		WriteOpCode(SLANG_OP_RECURSE);
	else if (terminating){
		if (!inlineSymHead){
			WriteOpCode(SLANG_OP_RETCALL);
		} else {
			WriteOpCode(SLANG_OP_RETCALLSYM);
			WriteInt16(((SlangObj*)head)->symbol);
		}
	} else {
		if (!inlineSymHead){
			WriteOpCode(SLANG_OP_CALL);
		} else {
			WriteOpCode(SLANG_OP_CALLSYM);
			WriteInt16(((SlangObj*)head)->symbol);
		}
	}
	
	AddCodeLocation(expr);
	
	return true;
}

#undef COMPILE_GLOBAL_SYM

void CodeWriter::MakeDefaultObjs(){
	constTrueObj = alloc.MakeBool(true);
	constFalseObj = alloc.MakeBool(false);
	constZeroObj = (SlangHeader*)alloc.MakeInt(0);
	constOneObj = (SlangHeader*)alloc.MakeInt(1);
	constEOFObj = alloc.MakeEOF();
	constElseObj = (SlangHeader*)alloc.MakeSymbol(SLANG_ELSE);
}

CodeWriter::CodeWriter(SlangParser& parser) 
	: parser(parser),
	  alloc(&memChain,CodeWriterObjAlloc),
	  memChain(32768){
	Reset();
	optimize = true;
}

CodeWriter::~CodeWriter(){
	for (const auto& block : lambdaCodes){
		free(block.start);
	}
	lambdaCodes.clear();
	
	curr = nullptr;
}

inline size_t CodeWriter::AllocNewBlock(size_t initSize){
	size_t index = lambdaCodes.size();
	CodeBlock& block = lambdaCodes.emplace_back();
	block.size = initSize;
	block.locData.Reserve(8);
	block.start = (uint8_t*)malloc(block.size);
	block.write = block.start;
	block.isVariadic = false;
	block.isPure = true;
	block.isClosure = false;
	block.moduleIndex = currModuleIndex;
	block.name = EMPTY_NAME;
	totalAlloc += initSize;
	return index;
}

void CodeWriter::Reset(){
	errors.clear();
	for (const auto& block : lambdaCodes){
		free(block.start);
	}
	lambdaCodes.clear();
	lambdaStack.clear();
	knownLambdaStack.clear();
	
	knownLambdaStack.emplace_back();
	
	currDefName = EMPTY_NAME;
	currSetName = EMPTY_NAME;
	currModuleIndex = 0;
	
	memChain.Reset();
	MakeDefaultObjs();
	totalAlloc = 0;
	size_t initSize = 4096;
	CodeBlock& global = lambdaCodes[AllocNewBlock(initSize)];
	global.isPure = false;
	curr = &global;
}

inline void CodeInterpreter::PushTry(){
	TryData& td = tryStack.PlaceBack();
	int32_t offset = *(int32_t*)(pc+OPCODE_SIZE);
	td.gotoAddr = pc+offset;
	td.stackSize = stack.size;
	td.argStackSize = argStack.size;
	td.funcStackSize = funcStack.size;
}

inline void CodeInterpreter::LoadTry(){
	TryData& td = tryStack.Back();
	pc = td.gotoAddr;
	assert(stack.size>=td.stackSize);
	assert(argStack.size>=td.argStackSize);
	assert(funcStack.size>=td.funcStackSize);
	stack.size = td.stackSize;
	argStack.size = td.argStackSize;
	funcStack.size = td.funcStackSize;
	errors.clear();
	lamEnv = nullptr;
	tryStack.PopBack();
	
	SlangObj* emptyMaybe = alloc.AllocateObj(SlangType::Maybe);
	emptyMaybe->maybe = nullptr;
	argStack.data[argStack.size++] = (SlangHeader*)emptyMaybe;
}

inline void CodeInterpreter::Call(size_t funcIndex,SlangEnv* env){
	FuncData& f = funcStack.data[funcStack.size++];
	f.funcIndex = funcIndex;
	f.argsFrame = stack.size-1;
	f.env = env;
	const CodeBlock& cb = codeWriter.lambdaCodes[funcIndex];
	f.globalEnv = modules.data[cb.moduleIndex].globalEnv;
	
	f.retAddr = pc+SlangOpSizes[*pc];
	uint8_t* code = cb.start;
	pc = code-SlangOpSizes[*pc];
}

inline void CodeInterpreter::RetCall(size_t funcIndex,SlangEnv* env){
	StackData& d = stack.Back();
	FuncData& f = funcStack.Back();
	f.funcIndex = funcIndex;
	f.env = env;
	const CodeBlock& cb = codeWriter.lambdaCodes[funcIndex];
	f.globalEnv = modules.data[cb.moduleIndex].globalEnv;
	uint8_t* code = cb.start;
	size_t argCount = GetArgCount();
	size_t lowerBase = stack.data[f.argsFrame].base;
	size_t upperBase = d.base;
	// copy args down
	for (size_t i=0;i<argCount;++i){
		argStack.data[lowerBase+i] = argStack.data[upperBase+i];
	}
	pc = code-SlangOpSizes[*pc];
	// manual pop frame
	stack.PopBack();
	argStack.size = lowerBase+argCount;
}

inline void CodeInterpreter::Recurse(){
	StackData& d = stack.Back();
	FuncData& f = funcStack.Back();
	size_t funcIndex = f.funcIndex;
	uint8_t* code = codeWriter.lambdaCodes[funcIndex].start;
	size_t argCount = GetArgCount();
	size_t lowerBase = stack.data[f.argsFrame].base;
	size_t upperBase = d.base;
	// copy args down
	for (size_t i=0;i<argCount;++i){
		argStack.data[lowerBase+i] = argStack.data[upperBase+i];
	}
	pc = code-SlangOpSizes[SLANG_OP_RECURSE];
	// manual pop frame
	stack.PopBack();
	argStack.size = lowerBase+argCount;
}

inline void CodeInterpreter::Return(SlangHeader* val){
	PopFrame();
	PushArg(val);
}

inline SlangEnv* CodeInterpreter::AllocateEnvs(size_t size){
	size_t blockCount = size/SLANG_ENV_BLOCK_SIZE;
	if (size%SLANG_ENV_BLOCK_SIZE!=0||blockCount==0) ++blockCount;
	
	if (blockCount==1){
		SlangEnv* env = alloc.AllocateEnv();
		return env;
	}
	SlangEnv* main = nullptr;
	SlangEnv* prev = nullptr;
	for (size_t i=0;i<blockCount;++i){
		SlangEnv* obj = alloc.AllocateEnv();
		if (i==0){
			main = obj;
			PushArg((SlangHeader*)main);
		} else {
			prev = (SlangEnv*)PopArg();
			prev->next = obj;
		}
		prev = obj;
		PushArg((SlangHeader*)prev);
	}
	PopArg();
	return (SlangEnv*)PopArg();
}

inline void CodeInterpreter::DefEnvSymbol(SlangEnv* e,SymbolName name,SlangHeader* val){
	PushArg((SlangHeader*)e);
	if (!e->DefSymbol(name,val)){
		PushArg(val);
		SlangEnv* newEnv = AllocateEnvs(1);
		newEnv->DefSymbol(name,PopArg());
		e = (SlangEnv*)PopArg();
		e->AddBlock(newEnv);
		return;
	}
	PopArg();
}

inline void CodeInterpreter::DefCurrEnvSymbol(SymbolName name,SlangHeader* val){
	SlangEnv* e = GetCurrEnv();
	if (!e->DefSymbol(name,val)){
		PushArg(val);
		SlangEnv* newEnv = AllocateEnvs(1);
		newEnv->DefSymbol(name,PopArg());
		e = GetCurrEnv();
		e->AddBlock(newEnv);
	}
}

inline bool CodeInterpreter::SetRecursiveSymbol(SymbolName name,SlangHeader* val){
	SlangHeader* t;
	SlangEnv* e = GetCurrEnv();
	while (e){
		if (e->GetSymbol(name,&t)){
			return e->SetSymbol(name,val);
		}
		e = e->parent;
	}
	return false;
}

size_t CodeInterpreter::GetPCOffset() const {
	return pc-codeWriter.lambdaCodes[funcStack.Back().funcIndex].start;
}

inline SlangLambda* CodeInterpreter::CreateLambda(size_t index){
	CodeBlock& block = codeWriter.lambdaCodes[index];
	SlangLambda* lam = alloc.AllocateLambda();
	//lam->header.varCount = 0;//block.params.size;
	if (block.isVariadic)
		lam->header.flags |= FLAG_VARIADIC;
	lam->funcIndex = index;
	if (block.isClosure){
		lam->header.flags |= FLAG_CLOSURE;
		PushArg((SlangHeader*)lam);
		SlangEnv* newEnv = AllocateEnvs(block.params.size);
		lam = (SlangLambda*)PopArg();
		lam->env = newEnv;
		lam->env->parent = GetCurrEnv();
		ParamData& pd = block.params;
		SlangEnv* envIt = newEnv;
		for (uint16_t i=0;i<pd.size;++i){
			auto& mappingData = envIt->mappings[i%SLANG_ENV_BLOCK_SIZE];
			mappingData.sym = pd.data[i];
			mappingData.obj = nullptr;
			++envIt->header.varCount;
			
			if (i%SLANG_ENV_BLOCK_SIZE==SLANG_ENV_BLOCK_SIZE-1){
				envIt = envIt->next;
			}
		}
	} else {
		lam->env = GetCurrEnv();
	}
	
	return lam;
}

inline SlangList* CodeInterpreter::MakeVariadicList(
	size_t start,
	size_t end
){
	if (start==end) return nullptr;
	SlangList* listHead = nullptr;
	SlangList* list = nullptr;
	SlangList* prev = nullptr;
	for (size_t i=start;i<end;++i){
		list = alloc.AllocateList();
		list->left = argStack.data[i];
		list->right = nullptr;
		
		if (i==start){
			listHead = list;
			PushArg((SlangHeader*)listHead);
		} else {
			prev = (SlangList*)PopArg();
			prev->right = (SlangHeader*)list;
		}
		
		prev = list;
		PushArg((SlangHeader*)prev);
	}
	PopArg();
	listHead = (SlangList*)PopArg();
	
	return listHead;
}

inline SlangVec* CodeInterpreter::MakeVecFromArgs(size_t start,size_t end){
	if (start==end){
		return alloc.AllocateVec(0);
	}
	SlangVec* v = alloc.AllocateVec(end-start);
	memcpy(v->storage->objs,&argStack.data[start],(end-start)*sizeof(SlangHeader*));
	return v;
}

SlangHeader* CodeInterpreter::Copy(SlangHeader* expr){
	if (!arena->InCurrSet((uint8_t*)expr)) return expr;
	
	PushArg(expr);
	size_t size = expr->GetSize();
	SlangHeader* copied = (SlangHeader*)CodeInterpreterAllocate(this,size);
	expr = PopArg();
	memcpy(copied,expr,size);
	
	return copied;
}

SlangList* CodeInterpreter::CopyList(SlangList* list){
	size_t lindex = argStack.size;
	size_t aindex = argStack.size+1;
	PushArg((SlangHeader*)list);
	SlangList* allocd = alloc.AllocateList();
	allocd->left = nullptr;
	allocd->right = nullptr;
	PushArg((SlangHeader*)allocd);
	PushArg(argStack.Back());
	while (true){
		((SlangList*)argStack.data[aindex])->left = 
			((SlangList*)argStack.data[lindex])->left;
		
		
		argStack.data[lindex] = ((SlangList*)argStack.data[lindex])->right;
		if (GetType(argStack.data[lindex])!=SlangType::List){
			((SlangList*)argStack.data[aindex])->right = argStack.data[lindex];
			break;
		}
		
		((SlangList*)argStack.data[aindex])->right = (SlangHeader*)alloc.AllocateList();
		argStack.data[aindex] = ((SlangList*)argStack.data[aindex])->right;
		((SlangList*)argStack.data[aindex])->left = nullptr;
		((SlangList*)argStack.data[aindex])->right = nullptr;
	}
	SlangList* retlist = (SlangList*)argStack.data[aindex+1];
	argStack.size -= 3;
	return retlist;
}

inline void CodeInterpreter::RehashDict(SlangDict* dict){
	size_t elemCount = dict->storage->size;
	SlangDictTable* table = dict->table;
	size_t cap = table->capacity;
	memset(table->elementOffsets,0xFF,sizeof(size_t)*cap);
	
	size_t* offset;
	const size_t* end = table->elementOffsets+cap;
	
	uint64_t hashVal;
	SlangDictElement* obj;
	for (size_t i=0;i<elemCount;++i){
		obj = &dict->storage->elements[i];
		if ((uint64_t)obj->key==DICT_UNOCCUPIED_VAL)
			continue;
		
		hashVal = obj->hash;
		
		offset = &table->elementOffsets[hashVal&(cap-1)];
		while (*offset!=DICT_UNOCCUPIED_VAL){
			++offset;
			if (offset==end)
				offset = table->elementOffsets;
		}
		
		*offset = i;
	}
}

inline void CodeInterpreter::RawDictInsertStorage(
		SlangDict* dict,
		uint64_t hash,
		SlangHeader* key,
		SlangHeader* val){
	
	if (dict->storage->size==dict->storage->capacity){
		PushArg(key);
		PushArg(val);
		PushArg((SlangHeader*)dict);
		
		SlangStorage* newStorage = alloc.AllocateDictStorage(dict->storage->capacity*2);
		dict = (SlangDict*)PopArg();
		
		SlangDictTable* table = dict->table;
		
		SlangDictElement* from = dict->storage->elements;
		SlangDictElement* to = newStorage->elements;
		size_t s = dict->storage->size;
		size_t realCount = 0;
		uint64_t hashVal;
		for (size_t i=0;i<s;++i){
			if ((uint64_t)from->key==DICT_UNOCCUPIED_VAL){
				++from;
			} else {
				if (realCount!=i){
					hashVal = from->hash;
					table->elementOffsets[hashVal & (table->capacity-1)] = realCount;
				}
				*to++ = *from++;
				++realCount;
			}
		}
		
		newStorage->size = realCount;
		dict->storage = newStorage;
		
		val = PopArg();
		key = PopArg();
	}
	
	SlangDictElement& elem = dict->storage->elements[dict->storage->size++];
	++dict->table->size;
	elem.hash = hash;
	elem.key = key;
	elem.val = val;
}

inline void CodeInterpreter::RawDictInsert(SlangDict* dict,SlangHeader* key,SlangHeader* val){
	assert(IsHashable(key));
	
	SlangDictTable* table = dict->table;
	SlangStorage* storage = dict->storage;
	assert(storage);
	
	uint64_t hash = SlangHashObj(key);
	
	size_t* offset = &table->elementOffsets[hash & (table->capacity-1)];
	const size_t* end = table->elementOffsets+table->capacity;
	SlangDictElement* obj;
	while (true){
		if (*offset == DICT_UNOCCUPIED_VAL){
			size_t offsetOff = offset-table->elementOffsets;
			RawDictInsertStorage(dict,hash,key,val);
			offset = table->elementOffsets+offsetOff;
			*offset = dict->storage->size-1;
			return;
		}
		
		obj = &storage->elements[*offset];
		if (hash==obj->hash && EqualObjs(key,obj->key)){
			obj->val = val;
			return;
		}
	
		++offset;
		if (offset==end)
			offset = table->elementOffsets;
	}
}

inline SlangDict* CodeInterpreter::ReallocDict(SlangDict* dict){
	PushArg((SlangHeader*)dict);
	size_t newCap = dict->table->capacity*2;
	SlangDictTable* newTable = alloc.AllocateDictTable(newCap);
	dict = (SlangDict*)PopArg();
	newTable->size = dict->table->size;
	dict->table = newTable;
	
	RehashDict(dict);
	return dict;
}

inline void CodeInterpreter::DictInsert(SlangDict* dict,SlangHeader* key,SlangHeader* val){
	assert(IsHashable(key));
	
	if (!dict->storage){
		PushArg((SlangHeader*)dict);
		PushArg(key);
		PushArg(val);
		SlangStorage* newStorage = alloc.AllocateDictStorage(2);
		newStorage->size = 0;
		PushArg((SlangHeader*)newStorage);
		SlangDictTable* table = alloc.AllocateDictTable(4);
		table->size = 0;
		memset(table->elementOffsets,0xFF,sizeof(size_t)*table->capacity);
		newStorage = (SlangStorage*)PopArg();
		
		val = PopArg();
		key = PopArg();
		dict = (SlangDict*)PopArg();
		dict->storage = newStorage;
		dict->table = table;
	} else if (dict->ShouldGrow()){
		PushArg(key);
		PushArg(val);
		dict = ReallocDict(dict);
		val = PopArg();
		key = PopArg();
	}
	RawDictInsert(dict,key,val);
}

inline SlangStr* CodeInterpreter::ReallocateStr(SlangStr* str,size_t newSize){
	newSize = QuantizeSize(newSize);
	SlangStorage* storage = str->storage;
	if (!storage){
		if (newSize==0) return str;
		
		PushArg((SlangHeader*)str);
		SlangStorage* newStorage = alloc.AllocateStorage(newSize,sizeof(uint8_t));
		
		newStorage->size = 0;
		str = (SlangStr*)PopArg();
		str->storage = newStorage;
		return str;
	}
	
	// shrink
	if (newSize<=storage->capacity){
		storage->capacity = newSize;
		storage->size = (storage->size > storage->capacity) ? storage->capacity : storage->size;
		return str;
	}
	
	PushArg((SlangHeader*)str);
	SlangStorage* newStorage = alloc.AllocateStorage(newSize,sizeof(uint8_t));
	str = (SlangStr*)PopArg();
	memcpy(&newStorage->data[0],&str->storage->data[0],str->storage->size*sizeof(uint8_t));
	newStorage->size = str->storage->size;
	str->storage = newStorage;
	return str;
}

inline SlangStream* CodeInterpreter::ReallocateStream(SlangStream* stream,size_t addLen){
	assert(stream->header.isFile==false);
	assert(stream->str);
	size_t currCap = GetStorageCapacity(stream->str->storage);
	size_t currSize = GetStorageSize(stream->str->storage);
	size_t sizeIncr = addLen-(currSize-stream->pos);
	sizeIncr = ((ssize_t)sizeIncr<0) ? 0 : sizeIncr;
	// enough room
	if (sizeIncr<=currCap-currSize){
		if (stream->str->storage)
			stream->str->storage->size += sizeIncr;
		return stream;
	}
	
	PushArg((SlangHeader*)stream);
	size_t newSize = (currCap+sizeIncr)*3/2;
	SlangStr* newStr = ReallocateStr(stream->str,newSize);
	newStr->storage->size = currSize+sizeIncr;
	stream = (SlangStream*)PopArg();
	assert(stream->str==newStr);
	stream->str = newStr;
	return stream;
}

bool CodeInterpreter::ParseSlangString(const SlangStr& code,SlangHeader** res){
	if (!code.storage||code.storage->size==0){
		return false;
	}
	
	std::string_view sv{(const char*)code.storage->data,code.storage->size};
	
	SlangAllocator savedAlloc = parser.alloc;
	parser.alloc = evalAlloc;
	parser.SetCodeString(sv);
	parser.currModule = -1U;
	SlangHeader* parsed;
	bool success = parser.ParseLine(&parsed);
	if (parser.token.type!=SlangTokenType::EndOfFile)
		success = false;
	parser.alloc = savedAlloc;
	if (!success||!parser.errors.empty())
		return false;
	*res = parsed;
	return true;
}

#define TYPE_CHECK_NUMERIC(expr) \
	if (!IsNumeric(expr)){ \
		c->TypeError2(GetType(expr),SlangType::Int,SlangType::Real); \
		return false; \
	}
	
#define TYPE_CHECK_STREAM(expr) \
	if (!IsStream(expr)){ \
		c->TypeError2(GetType(expr),SlangType::InputStream,SlangType::OutputStream); \
		return false; \
	}
	
#define TYPE_CHECK_EXACT(expr,type) \
	if (GetType(expr)!=(type)){ \
		c->TypeError(GetType(expr),(type)); \
		return false; \
	}

bool ExtFuncGCManualCollect(CodeInterpreter* c){
	c->SmallGC(0);
	c->Return(nullptr);
	return true;
}

bool ExtFuncGCRecObjSize(CodeInterpreter* c){
	SlangHeader* val = c->GetArg(0);
		
	if (val==nullptr){
		c->Return(c->codeWriter.constZeroObj);
		return true;
	}
	
	c->Return((SlangHeader*)c->alloc.MakeInt(GetRecursiveSize(val)));
	return true;
}

bool ExtFuncGCObjSize(CodeInterpreter* c){
	SlangHeader* val = c->GetArg(0);
		
	if (val==nullptr){
		c->Return(c->codeWriter.constZeroObj);
		return true;
	}
	
	size_t count = 0;
	switch (val->type){
		case SlangType::Vector:
			count = val->GetSize();
			count += ((SlangVec*)val)->storage->header.GetSize();
			break;
		case SlangType::String:
			count = val->GetSize();
			count += ((SlangStr*)val)->storage->header.GetSize();
			break;
		case SlangType::Dict:
			count = val->GetSize();
			count += ((SlangDict*)val)->table->header.GetSize();
			count += ((SlangDict*)val)->storage->header.GetSize();
			break;
		default:
			count = val->GetSize();
			break;
	}
	
	c->Return((SlangHeader*)c->alloc.MakeInt(count));
	return true;
}

bool ExtFuncGCMemSize(CodeInterpreter* c){
	size_t size = c->arena->currPointer - c->arena->currSet;
	c->Return((SlangHeader*)c->alloc.MakeInt(size));
	return true;
}

bool ExtFuncGCMemCapacity(CodeInterpreter* c){
	size_t size = c->arena->memSize/2;
	c->Return((SlangHeader*)c->alloc.MakeInt(size));
	return true;
}

bool ExtFuncGCCodeMemSize(CodeInterpreter* c){
	size_t size = c->codeWriter.totalAlloc;
	c->Return((SlangHeader*)c->alloc.MakeInt(size));
	return true;
}

bool ExtFuncGetTime(CodeInterpreter* c){
	c->Return((SlangHeader*)c->alloc.MakeReal(GetDoubleTime()));
	return true;
}

bool ExtFuncGetPerfTime(CodeInterpreter* c){
	c->Return((SlangHeader*)c->alloc.MakeReal(GetDoublePerfTime()));
	return true;
}

bool ExtFuncRand(CodeInterpreter* c){
	c->Return((SlangHeader*)c->alloc.MakeInt(gRandState.Get64()));
	return true;
}

bool ExtFuncRandSeed(CodeInterpreter* c){
	SlangHeader* seedObj = c->GetArg(0);
	
	TYPE_CHECK_EXACT(seedObj,SlangType::Int);
	gRandState.Seed(((SlangObj*)seedObj)->integer);
	c->Return(nullptr);
	return true;
}

void ExtFileFinalizer(CodeInterpreter*,SlangHeader* obj){
	assert(obj->type==SlangType::InputStream||obj->type==SlangType::OutputStream);
	SlangStream* stream = (SlangStream*)obj;
	assert(stream->header.isFile);
	if (stream->file){
		fclose(stream->file);
		stream->file = NULL;
	}
}

bool ExtFuncFileClose(CodeInterpreter* c){
	SlangHeader* streamObj = c->GetArg(0);
	
	TYPE_CHECK_STREAM(streamObj);
	
	if (!streamObj->isFile){
		c->StreamError("Stream is not a file stream!");
		return false;
	}
	
	SlangStream* stream = (SlangStream*)streamObj;
	if (stream->file){
		fclose(stream->file);
		stream->file = NULL;
	}
	
	c->RemoveFinalizer(streamObj);
	
	c->Return(nullptr);
	return true;
}

SlangHeader* ExtFuncStdIn(CodeInterpreter* c){
	SlangStream* stream = c->alloc.AllocateStream(SlangType::InputStream);
	stream->header.isFile = true;
	stream->pos = 0;
	stream->file = stdin;
	return (SlangHeader*)stream;
}

SlangHeader* ExtFuncStdOut(CodeInterpreter* c){
	SlangStream* stream = c->alloc.AllocateStream(SlangType::OutputStream);
	stream->header.isFile = true;
	stream->pos = 0;
	stream->file = stdout;
	return (SlangHeader*)stream;
}

SlangHeader* ExtFuncStdErr(CodeInterpreter* c){
	SlangStream* stream = c->alloc.AllocateStream(SlangType::OutputStream);
	stream->header.isFile = true;
	stream->pos = 0;
	stream->file = stderr;
	return (SlangHeader*)stream;
}

bool ExtGetFilename(CodeInterpreter* c,std::string& str){
	SlangHeader* strObj = c->GetArg(0);
	
	TYPE_CHECK_EXACT(strObj,SlangType::String);
	
	SlangStr* filename = (SlangStr*)strObj;
	if (GetStorageSize(filename->storage)==0){
		c->FileError("Could not open file with empty filename!");
		return false;
	}
	
	str.resize(filename->storage->size);
	memcpy(str.data(),&filename->storage->data[0],sizeof(uint8_t)*filename->storage->size);
	return true;
}

bool ExtFuncFileOpenWithMode(
		CodeInterpreter* c,
		const char* mode,
		SlangType streamType){
	std::string str{};
	if (!ExtGetFilename(c,str))
		return false;
	
	FILE* f = fopen(str.c_str(),mode);
	if (!f){
		std::stringstream ss{};
		ss << "Could not open path '" << str << "'";
		c->FileError(ss.str());
		return false;
	}
	
	SlangStream* stream = c->alloc.AllocateStream(streamType);
	stream->header.isFile = true;
	stream->file = f;
	stream->pos = 0;
	
	c->AddFinalizer({(SlangHeader*)stream,&ExtFileFinalizer});
	c->Return((SlangHeader*)stream);
	return true;
}

bool ExtFuncFileOpenRead(CodeInterpreter* c){
	return ExtFuncFileOpenWithMode(c,"r",SlangType::InputStream);
}

bool ExtFuncFileOpenWrite(CodeInterpreter* c){
	return ExtFuncFileOpenWithMode(c,"w",SlangType::OutputStream);
}

bool ExtFuncFileOpenWriteApp(CodeInterpreter* c){
	return ExtFuncFileOpenWithMode(c,"a",SlangType::OutputStream);
}

bool ExtFuncFileFlush(CodeInterpreter* c){
	SlangHeader* streamObj = c->GetArg(0);
	
	TYPE_CHECK_STREAM(streamObj);
	
	if (!streamObj->isFile){
		c->Return(streamObj);
		return true;
	}
	
	SlangStream* stream = (SlangStream*)streamObj;
	if (!stream->file){
		c->FileError("Cannot flush a closed file!");
		return false;
	}
	
	fflush(stream->file);
	c->Return((SlangHeader*)stream);
	return true;
}

bool ExtFuncIsFile(CodeInterpreter* c){
	SlangHeader* streamObj = c->GetArg(0);
	
	SlangType sType = GetType(streamObj);
	if (sType!=SlangType::InputStream && sType!=SlangType::OutputStream){
		c->Return(c->codeWriter.constFalseObj);
		return true;
	}
	
	if (streamObj->isFile){
		c->Return(c->codeWriter.constTrueObj);
	} else {
		c->Return(c->codeWriter.constFalseObj);
	}
	return true;
}

bool ExtFuncIsFileOpen(CodeInterpreter* c){
	SlangHeader* streamObj = c->GetArg(0);
	
	TYPE_CHECK_STREAM(streamObj);
	
	if (!streamObj->isFile){
		c->StreamError("Stream is not a file stream!");
		return false;
	}
	
	if (((SlangStream*)streamObj)->file!=NULL){
		c->Return(c->codeWriter.constTrueObj);
	} else {
		c->Return(c->codeWriter.constFalseObj);
	}
	return true;
}

bool ExtFuncPathExists(CodeInterpreter* c){
	SlangHeader* pathObj = c->GetArg(0);
	TYPE_CHECK_EXACT(pathObj,SlangType::String);
	SlangStr* str = (SlangStr*)pathObj;
	
	if (!str->storage){
		c->Return(c->codeWriter.constFalseObj);
		return true;
	}
	
	std::string copied{};
	copied.resize(str->storage->size);
	memcpy(copied.data(),str->storage->data,str->storage->size);
	
	if (CheckFileExists(copied)){
		c->Return(c->codeWriter.constTrueObj);
	} else {
		c->Return(c->codeWriter.constFalseObj);
	}
	return true;
}

bool ExtFuncPathRemove(CodeInterpreter* c){
	SlangHeader* pathObj = c->GetArg(0);
	TYPE_CHECK_EXACT(pathObj,SlangType::String);
	SlangStr* str = (SlangStr*)pathObj;
	
	if (!str->storage){
		c->FileError("Could not remove path ''");
		return false;
	}
	
	std::string copied{};
	copied.resize(str->storage->size);
	memcpy(copied.data(),str->storage->data,str->storage->size);
	
	if (!CheckFileExists(copied)){
		std::stringstream ss{};
		ss << "Path '" << copied << "' does not exist!";
		c->FileError(ss.str());
		return false;
	}
	
	if (!TryRemoveFile(copied)){
		std::stringstream ss{};
		ss << "Could not remove path '" << copied << "'";
		c->FileError(ss.str());
		return false;
	}
	
	c->Return(nullptr);
	return true;
}

typedef bool(*CodeFunc)(CodeInterpreter* c);
typedef SlangHeader*(*ExtVarCreateFunc)(CodeInterpreter* c);

struct ExternalFuncData {
	const char* name;
	CodeFunc func;
	uint32_t minArgs;
	uint32_t maxArgs;
	uint8_t purity;
};

struct ExternalVarData {
	const char* name;
	ExtVarCreateFunc createFunc;
};

struct BuiltinModuleData {
	const char* name;
	
	const ExternalFuncData* funcs;
	size_t fCount;
	const ExternalVarData* vars;
	size_t vCount;
};

const ExternalFuncData moduleFileFuncs[] = {
	{
		"make-ifstream",
		&ExtFuncFileOpenRead,
		1,
		1,
		SLANG_HEAD_PURE
	},
	{
		"make-ofstream!",
		&ExtFuncFileOpenWrite,
		1,
		1,
		SLANG_IMPURE
	},
	{
		"make-ofstream-app!",
		&ExtFuncFileOpenWriteApp,
		1,
		1,
		SLANG_IMPURE
	},
	{
		"file-close!",
		&ExtFuncFileClose,
		1,
		1,
		SLANG_IMPURE
	},
	{
		"flush!",
		&ExtFuncFileFlush,
		1,
		1,
		SLANG_IMPURE
	},
	{
		"file?",
		&ExtFuncIsFile,
		1,
		1,
		SLANG_HEAD_PURE
	},
	{
		"file-open?",
		&ExtFuncIsFileOpen,
		1,
		1,
		SLANG_HEAD_PURE
	},
	{
		"path-exists?",
		&ExtFuncPathExists,
		1,
		1,
		SLANG_HEAD_PURE
	},
	{
		"path-remove!",
		&ExtFuncPathRemove,
		1,
		1,
		SLANG_IMPURE
	}
};

const ExternalVarData moduleFileVars[] = {
	{
		"stdin",
		&ExtFuncStdIn
	},
	{
		"stdout",
		&ExtFuncStdOut
	},
	{
		"stderr",
		&ExtFuncStdErr
	}
};

const ExternalFuncData moduleTimeFuncs[] = {
	{
		"time",
		&ExtFuncGetTime,
		0,
		0,
		SLANG_PURE
	},
	{
		"perf-time",
		&ExtFuncGetPerfTime,
		0,
		0,
		SLANG_PURE
	},
};

const ExternalFuncData moduleRandomFuncs[] = {
	{
		"rand!",
		&ExtFuncRand,
		0,
		0,
		SLANG_IMPURE
	},
	{
		"rand-seed!",
		&ExtFuncRandSeed,
		1,
		1,
		SLANG_IMPURE
	},
};

const ExternalFuncData moduleGCFuncs[] = {
	{
		"gc-collect",
		&ExtFuncGCManualCollect,
		0,
		0,
		SLANG_IMPURE
	},
	{
		"gc-rec-size",
		&ExtFuncGCRecObjSize,
		1,
		1,
		SLANG_HEAD_PURE
	},
	{
		"gc-size",
		&ExtFuncGCObjSize,
		1,
		1,
		SLANG_HEAD_PURE
	},
	{
		"gc-mem-size",
		&ExtFuncGCMemSize,
		0,
		0,
		SLANG_HEAD_PURE
	},
	{
		"gc-mem-cap",
		&ExtFuncGCMemCapacity,
		0,
		0,
		SLANG_HEAD_PURE
	},
	{
		"gc-code-size",
		&ExtFuncGCCodeMemSize,
		0,
		0,
		SLANG_HEAD_PURE
	},
};

const BuiltinModuleData SlangBuiltinModules[] = {
	{
		"file",
		moduleFileFuncs,
		SL_ARR_LEN(moduleFileFuncs),
		moduleFileVars,
		SL_ARR_LEN(moduleFileVars)
	},
	{
		"time",
		moduleTimeFuncs,
		SL_ARR_LEN(moduleTimeFuncs),
		NULL,
		0
	},
	{
		"random",
		moduleRandomFuncs,
		SL_ARR_LEN(moduleRandomFuncs),
		NULL,
		0
	},
	{
		"gc",
		moduleGCFuncs,
		SL_ARR_LEN(moduleGCFuncs),
		NULL,
		0
	},
};
#define BUILTIN_MODULE_COUNT SL_ARR_LEN(SlangBuiltinModules)

bool GoodExtArity(const ExternalFuncData* efd,size_t argCount){
	if (argCount<(size_t)efd->minArgs||argCount>(size_t)efd->maxArgs){
		return false;
	}
	return true;
}

void CodeInterpreter::InitBuiltinModules(){
	builtinModulesMap.reserve(BUILTIN_MODULE_COUNT);
	
	SlangAllocator savedAlloc = alloc;
	alloc = constAlloc;
	
	for (size_t i=0;i<BUILTIN_MODULE_COUNT;++i){
		const BuiltinModuleData& bmd = SlangBuiltinModules[i];
		SymbolName sym = parser.RegisterSymbol(bmd.name);
		SlangEnv* env = AllocateEnvs(bmd.fCount+bmd.vCount);
		for (size_t j=0;j<bmd.fCount;++j){
			const ExternalFuncData& efd = bmd.funcs[j];
			SymbolName funcSym = parser.RegisterSymbol(efd.name);
			SlangLambda* lam = alloc.AllocateLambda();
			lam->header.flags |= FLAG_EXTERNAL;
			lam->extFunc = &efd;
			env->DefSymbol(funcSym,(SlangHeader*)lam);
		}
		for (size_t j=0;j<bmd.vCount;++j){
			const ExternalVarData& evd = bmd.vars[j];
			SymbolName varName = parser.RegisterSymbol(evd.name);
			SlangHeader* val = evd.createFunc(this);
			env->DefSymbol(varName,val);
		}
		
		builtinModulesMap[sym] = env;
	}
	
	alloc = savedAlloc;
}

bool CodeInterpreter::ImportBuiltinModule(SymbolName name){
	if (!builtinModulesMap.contains(name))
		return false;
	
	SlangEnv* importEnv = builtinModulesMap.at(name);
	ImportEnv(importEnv);
	return true;
}

inline void CodeInterpreter::AddFinalizer(const Finalizer& f){
	assert(f.obj);
	finalizers.PushBack(f);
}

inline void CodeInterpreter::RemoveFinalizer(SlangHeader* obj){
	for (ssize_t i=finalizers.size-1;i>=0;--i){
		if (obj==finalizers.data[i].obj){
			finalizers.data[i] = finalizers.data[--finalizers.size];
			break;
		}
	}
}

inline SlangStream* CodeInterpreter::MakeStringInputStream(SlangStr* str){
	PushArg((SlangHeader*)str);
	SlangStream* stream = alloc.AllocateStream(SlangType::InputStream);
	stream->header.isFile = false;
	stream->pos = 0;
	stream->str = (SlangStr*)PopArg();
	return stream;
}

inline SlangStream* CodeInterpreter::MakeStringOutputStream(SlangStr* str){
	SlangStream* stream;
	if (!str){
		str = alloc.AllocateStr(8);
		str->storage->size = 0;
		stream = alloc.AllocateStream(SlangType::OutputStream);
		stream->str = str;
		stream->pos = 0;
	} else {
		PushArg((SlangHeader*)str);
		stream = alloc.AllocateStream(SlangType::OutputStream);
		str = (SlangStr*)PopArg();
		stream->str = str;
		stream->pos = GetStorageSize(str->storage);
	}
	stream->header.isFile = false;
	
	return stream;
}

inline void CodeInterpreter::ImportEnv(const SlangEnv* env){
	if (!env)
		return;
	SlangEnv* currEnv = GetCurrEnv();
	PushArg((SlangHeader*)env);
	while (true){
		for (size_t i=0;i<env->header.varCount;++i){
			SymbolName sym = env->mappings[i].sym;
			if (!currEnv->HasSymbol(sym)){
				DefEnvSymbol(currEnv,sym,env->mappings[i].obj);
				currEnv = GetCurrEnv();
				env = (SlangEnv*)PeekArg();
			}
		}
		if (!env->next) break;
		env = env->next;
		argStack.data[argStack.size-1] = (SlangHeader*)env;
	}
	PopArg();
}

inline bool CodeInterpreter::SlangOutputToFile(FILE* file,SlangHeader* obj){
	switch (GetType(obj)){
		case SlangType::String: {
			SlangStr* str = (SlangStr*)obj;
			if (str->storage){
				fwrite(str->storage->data,str->storage->size,1,file);
			}
			break;
		}
		case SlangType::Int: {
			SlangObj* intObj = (SlangObj*)obj;
			fprintf(file,"%ld",intObj->integer);
			break;
		}
		case SlangType::Real: {
			SlangObj* realObj = (SlangObj*)obj;
			fprintf(file,"%.16g",realObj->real);
			break;
		}
		case SlangType::Symbol: {
			SlangObj* symObj = (SlangObj*)obj;
			std::string_view symStr = parser.GetSymbolString(symObj->symbol);
			fwrite(symStr.data(),symStr.size(),1,file);
			break;
		}
		case SlangType::EndOfFile:
			break;
		default:
			TypeError(GetType(obj),SlangType::String);
			return false;
	}
	return true;
}

inline SlangHeader* CodeInterpreter::SlangInputFromFile(FILE* file){
	std::string temp{};
	temp.reserve(256);
	int c;
	while ((c = fgetc(file))!=EOF){
		if (c=='\n') break;
		temp.push_back(c);
	}
	
	if (temp.empty()&&c==EOF){
		return alloc.MakeEOF();
	}
	
	SlangStr* str = alloc.AllocateStr(temp.size());
	if (temp.empty()) return (SlangHeader*)str;
	memcpy(&str->storage->data[0],temp.data(),temp.size());
	return (SlangHeader*)str;
}

inline bool CodeInterpreter::SlangOutputToString(SlangStream* stream,SlangHeader* obj){
	static char tempStr[64];
	
	SlangStr* sstr = stream->str;
	switch (GetType(obj)){
		case SlangType::String: {
			SlangStr* str = (SlangStr*)obj;
			if (GetStorageSize(str->storage)==0) break;
			
			PushArg((SlangHeader*)str);
			stream = ReallocateStream(stream,str->storage->size);
			sstr = stream->str;
			str = (SlangStr*)PopArg();
			// stream is now guaranteed big enough
			
			
			uint8_t* it = sstr->storage->data+stream->pos;
			uint8_t* otherIt = str->storage->data;
			memcpy(it,otherIt,str->storage->size);
			//for (size_t i=0;i<str->storage->size;++i){
				//*it++ = *otherIt++;
			//}
			
			stream->pos += str->storage->size;
			break;
		}
		case SlangType::Int: {
			SlangObj* intObj = (SlangObj*)obj;
			ssize_t size = snprintf(tempStr,sizeof(tempStr),"%ld",intObj->integer);
			assert(size!=-1);
			stream = ReallocateStream(stream,size);
			sstr = stream->str;
			
			uint8_t* it = sstr->storage->data+stream->pos;
			uint8_t* otherIt = (uint8_t*)&tempStr[0];
			for (ssize_t i=0;i<size;++i){
				*it++ = *otherIt++;
			}
			
			stream->pos += size;
			break;
		}
		case SlangType::Real: {
			SlangObj* realObj = (SlangObj*)obj;
			ssize_t size = snprintf(tempStr,sizeof(tempStr),"%.16g",realObj->real);
			assert(size!=-1);
			stream = ReallocateStream(stream,size);
			sstr = stream->str;
			
			uint8_t* it = sstr->storage->data+stream->pos;
			uint8_t* otherIt = (uint8_t*)&tempStr[0];
			for (ssize_t i=0;i<size;++i){
				*it++ = *otherIt++;
			}
			
			stream->pos += size;
			break;
		}
		case SlangType::Symbol: {
			SlangObj* symObj = (SlangObj*)obj;
			std::string_view symStr = parser.GetSymbolString(symObj->symbol);
			ssize_t size = symStr.size();
			stream = ReallocateStream(stream,size);
			sstr = stream->str;
			
			uint8_t* it = sstr->storage->data+stream->pos;
			uint8_t* otherIt = (uint8_t*)symStr.data();
			memcpy(it,otherIt,size);
			/*for (ssize_t i=0;i<size;++i){
				*it++ = *otherIt++;
			}*/
			
			stream->pos += size;
			break;
		}
		case SlangType::EndOfFile:
			break;
		default:
			TypeError(GetType(obj),SlangType::String);
			return false;
	}
	return true;
}

inline SlangHeader* CodeInterpreter::SlangInputFromString(SlangStream* stream){
	std::string temp{};
	temp.reserve(256);
	uint8_t c;
	size_t strSize = GetStorageSize(stream->str->storage);
	if (!strSize||stream->pos>=strSize){
		return alloc.MakeEOF();
	}
	
	while (true){
		c = stream->str->storage->data[stream->pos++];
		if (c=='\n')
			break;
		temp.push_back(c);
		if (stream->pos>=strSize) break;
	}
	
	if (temp.empty()&&stream->pos>=strSize){
		return alloc.MakeEOF();
	}
	
	SlangStr* str = alloc.AllocateStr(temp.size());
	if (temp.empty()) return (SlangHeader*)str;
	memcpy(&str->storage->data[0],temp.data(),temp.size());
	return (SlangHeader*)str;
}

void CodeInterpreter::SetGlobalSymbol(const std::string& name,SlangHeader* val){
	SymbolName sym = parser.RegisterSymbol(name);
	SlangEnv* globalEnv = funcStack.data[0].env;
	if (!globalEnv->SetSymbol(sym,val)){
		DefEnvSymbol(globalEnv,sym,val);
	}
}

bool CodeInterpreter::EvalCall(SlangHeader* codeObj){
	//evalMemChain.Reset();
	SlangAllocator savedAlloc = codeWriter.alloc;
	codeWriter.alloc = evalAlloc;
	codeWriter.currModuleIndex = codeWriter.lambdaCodes[funcStack.Back().funcIndex].moduleIndex;
	bool r = codeWriter.CompileData(codeObj);
	codeWriter.alloc = savedAlloc;
	
	if (r){
		size_t index = codeWriter.evalFuncIndex;
		Call(index,funcStack.Back().env);
	}
	return r;
}

typedef bool(*CodeWalkFunc)(const uint8_t* c,void* data);
void CodeWalker(const uint8_t* code,const uint8_t* end,CodeWalkFunc func,void* data){
	while (code<end){
		if (!func(code,data))
			break;
		code += SlangOpSizes[*code];
	}
}

struct PrintCodeData {
	size_t pos;
	const uint8_t* pc;
};

bool PrintCodeSub(const uint8_t* c,void* data){
	SlangHeader* ptr;
	SymbolName sym;
	PrintCodeData* dat = (PrintCodeData*)data;
	uint16_t localIdx;
	int32_t big;
	uint64_t lamIndex;
	char spacer = ' ';
	if (c==dat->pc) spacer = '*';
	std::cout << dat->pos << ' ';
	if (dat->pos<1000) std::cout << spacer;
	if (dat->pos<100) std::cout << spacer;
	if (dat->pos<10) std::cout << spacer;
	
	switch (*c){
		case SLANG_OP_NOOP:
			std::cout << "NOOP\n";
			break;
		case SLANG_OP_HALT:
			std::cout << "HALT\n";
			break;
		case SLANG_OP_NULL:
			std::cout << "PUSH ()\n";
			break;
		case SLANG_OP_LOAD_PTR:
			std::cout << "LD ";
			ptr = *(SlangHeader**)(c+OPCODE_SIZE);
			if (!ptr){
				std::cout << "()";
			} else {
				std::cout << *ptr;
			}
			
			std::cout << '\n';
			break;
		case SLANG_OP_BOOL_TRUE:
			std::cout << "PUSH TRUE\n";
			break;
		case SLANG_OP_BOOL_FALSE:
			std::cout << "PUSH FALSE\n";
			break;
		case SLANG_OP_ZERO:
			std::cout << "PUSH ZERO\n";
			break;
		case SLANG_OP_ONE:
			std::cout << "PUSH ONE\n";
			break;
		case SLANG_OP_LOOKUP:
			std::cout << "LOOKUP ";
			sym = *(SymbolName*)(c+OPCODE_SIZE);
			std::cout << gDebugParser->GetSymbolString(sym);
			std::cout << '\n';
			break;
		case SLANG_OP_SET:
			std::cout << "SET ";
			sym = *(SymbolName*)(c+OPCODE_SIZE);
			std::cout << gDebugParser->GetSymbolString(sym);
			std::cout << '\n';
			break;
		case SLANG_OP_GET_LOCAL:
			std::cout << "GETLOCAL ";
			localIdx = *(uint16_t*)(c+OPCODE_SIZE);
			std::cout << localIdx;
			std::cout << '\n';
			break;
		case SLANG_OP_SET_LOCAL:
			std::cout << "SETLOCAL ";
			localIdx = *(uint16_t*)(c+OPCODE_SIZE);
			std::cout << localIdx;
			std::cout << '\n';
			break;
		case SLANG_OP_GET_GLOBAL:
			std::cout << "GETGLOBAL ";
			sym = *(SymbolName*)(c+OPCODE_SIZE);
			std::cout << gDebugParser->GetSymbolString(sym);
			std::cout << '\n';
			break;
		case SLANG_OP_SET_GLOBAL:
			std::cout << "SETGLOBAL ";
			sym = *(SymbolName*)(c+OPCODE_SIZE);
			std::cout << gDebugParser->GetSymbolString(sym);
			std::cout << '\n';
			break;
		case SLANG_OP_DEF_GLOBAL:
			std::cout << "DEFGLOBAL ";
			sym = *(SymbolName*)(c+OPCODE_SIZE);
			std::cout << gDebugParser->GetSymbolString(sym);
			std::cout << '\n';
			break;
		case SLANG_OP_GET_REC:
			std::cout << "GETREC ";
			localIdx = *(uint16_t*)(c+OPCODE_SIZE);
			std::cout << localIdx;
			std::cout << '\n';
			break;
		case SLANG_OP_SET_REC:
			std::cout << "SETREC ";
			localIdx = *(uint16_t*)(c+OPCODE_SIZE);
			std::cout << localIdx;
			std::cout << '\n';
			break;
		case SLANG_OP_PUSH_FRAME:
			std::cout << "PUSH FRAME\n";
			break;
		case SLANG_OP_PUSH_LAMBDA:
			std::cout << "PUSH LAMBDA ";
			lamIndex = *(uint64_t*)(c+OPCODE_SIZE);
			std::cout << lamIndex << '\n';
			break;
		case SLANG_OP_POP_ARG:
			std::cout << "POP\n";
			break;
		case SLANG_OP_UNPACK:
			std::cout << "UNPACK\n";
			break;
		case SLANG_OP_COPY:
			std::cout << "COPY\n";
			break;
		case SLANG_OP_CALL:
			std::cout << "CALL\n";
			break;
		case SLANG_OP_CALLSYM:
			std::cout << "CALLSYM ";
			localIdx = *(uint16_t*)(c+OPCODE_SIZE);
			std::cout << gDebugParser->GetSymbolString((SymbolName)localIdx);
			std::cout << '\n';
			break;
		case SLANG_OP_RET:
			std::cout << "RET\n";
			break;
		case SLANG_OP_RETCALL:
			std::cout << "RETCALL\n";
			break;
		case SLANG_OP_RETCALLSYM:
			std::cout << "RETCALLSYM ";
			localIdx = *(uint16_t*)(c+OPCODE_SIZE);
			std::cout << gDebugParser->GetSymbolString((SymbolName)localIdx);
			std::cout << '\n';
			break;
		case SLANG_OP_RECURSE:
			std::cout << "RECURSE\n";
			break;
		case SLANG_OP_JUMP:
			std::cout << "JMP ";
			big = *(int32_t*)(c+OPCODE_SIZE);
			big += dat->pos;
			std::cout << big << '\n';
			break;
		case SLANG_OP_CJUMP_POP:
			std::cout << "POP CJMP ";
			big = *(int32_t*)(c+OPCODE_SIZE);
			big += dat->pos;
			std::cout << big << '\n';
			break;
		case SLANG_OP_CNJUMP_POP:
			std::cout << "POP CNJP ";
			big = *(int32_t*)(c+OPCODE_SIZE);
			big += dat->pos;
			std::cout << big << '\n';
			break;
		case SLANG_OP_CJUMP:
			std::cout << "CJMP ";
			big = *(int32_t*)(c+OPCODE_SIZE);
			big += dat->pos;
			std::cout << big << '\n';
			break;
		case SLANG_OP_CNJUMP:
			std::cout << "CNJP ";
			big = *(int32_t*)(c+OPCODE_SIZE);
			big += dat->pos;
			std::cout << big << '\n';
			break;
		case SLANG_OP_CASE_JUMP:
			std::cout << "CASEJUMP ";
			big = *(int32_t*)(c+OPCODE_SIZE);
			std::cout << big << '\n';
			break;
		case SLANG_OP_TRY:
			std::cout << "TRY ";
			big = *(int32_t*)(c+OPCODE_SIZE);
			big += dat->pos;
			std::cout << big << '\n';
			break;
		case SLANG_OP_MAYBE_NULL:
			std::cout << "PUSH ?<>\n";
			break;
		case SLANG_OP_MAYBE_WRAP:
			std::cout << "MAYBE WRAP\n";
			break;
		case SLANG_OP_MAYBE_UNWRAP:
			std::cout << "UNWRAP\n";
			break;
		case SLANG_OP_MAP_STEP:
			std::cout << "MAPSTEP ";
			localIdx = *(uint16_t*)(c+OPCODE_SIZE);
			std::cout << localIdx << '\n';
			break;
		case SLANG_OP_NOT:
			std::cout << "NOT\n";
			break;
		case SLANG_OP_INC:
			std::cout << "INC\n";
			break;
		case SLANG_OP_DEC:
			std::cout << "DEC\n";
			break;
		case SLANG_OP_NEG:
			std::cout << "NEG\n";
			break;
		case SLANG_OP_INVERT:
			std::cout << "INVERT\n";
			break;
		case SLANG_OP_ADD:
			std::cout << "ADD ";
			localIdx = *(uint16_t*)(c+OPCODE_SIZE);
			std::cout << localIdx << '\n';
			break;
		case SLANG_OP_SUB:
			std::cout << "SUB ";
			localIdx = *(uint16_t*)(c+OPCODE_SIZE);
			std::cout << localIdx << '\n';
			break;
		case SLANG_OP_MUL:
			std::cout << "MUL ";
			localIdx = *(uint16_t*)(c+OPCODE_SIZE);
			std::cout << localIdx << '\n';
			break;
		case SLANG_OP_DIV:
			std::cout << "DIV ";
			localIdx = *(uint16_t*)(c+OPCODE_SIZE);
			std::cout << localIdx << '\n';
			break;
		case SLANG_OP_EQ:
			std::cout << "EQ\n";
			break;
		case SLANG_OP_PAIR:
			std::cout << "PAIR\n";
			break;
		case SLANG_OP_LIST_CONCAT:
			std::cout << "LCONCAT ";
			localIdx = *(uint16_t*)(c+OPCODE_SIZE);
			std::cout << localIdx << '\n';
			break;
		case SLANG_OP_LEFT:
			std::cout << "L\n";
			break;
		case SLANG_OP_RIGHT:
			std::cout << "R\n";
			break;
		case SLANG_OP_SET_LEFT:
			std::cout << "SETL\n";
			break;
		case SLANG_OP_SET_RIGHT:
			std::cout << "SETR\n";
			break;
		case SLANG_OP_MAKE_VEC:
			std::cout << "MAKE VEC\n";
			break;
		case SLANG_OP_VEC_GET:
			std::cout << "VECGET\n";
			break;
		case SLANG_OP_VEC_SET:
			std::cout << "VECSET\n";
			break;
		case SLANG_OP_EXPORT:
			std::cout << "EXPORT ";
			sym = *(SymbolName*)(c+OPCODE_SIZE);
			std::cout << gDebugParser->GetSymbolString(sym);
			std::cout << '\n';
			break;
		case SLANG_OP_IMPORT:
			std::cout << "IMPORT ";
			ptr = *(SlangHeader**)(c+OPCODE_SIZE);
			if (ptr)
				std::cout << *ptr;
			else
				std::cout << "()";
			std::cout << '\n';
			break;
		default:
			std::cout << "??\n";
			break;
	}
	dat->pos += SlangOpSizes[*c];
	return true;
}

void PrintCode(const uint8_t* code,const uint8_t* end,const uint8_t* pc){
	PrintCodeData dat{};
	dat.pos = 0;
	dat.pc = pc;
	CodeWalker(code,end,PrintCodeSub,&dat);
}

void CodeInterpreter::DisplayErrors() const {
	if (!parser.errors.empty()){
		std::cout << "Encountered errors while parsing:\n";
		PrintErrors(parser.errors);
	}
	if (!codeWriter.errors.empty()){
		std::cout << "Encountered errors while compiling:\n";
		PrintErrors(codeWriter.errors);
	}
	if (!errors.empty()){
		std::cout << "Encountered errors while evaluating:\n";
		PrintErrors(errors);
	}
}

void CodeInterpreter::PushError(const std::string& type,const std::string& msg){
	size_t currFuncIndex = funcStack.Back().funcIndex;
	const CodeBlock& cb = codeWriter.lambdaCodes[currFuncIndex];
	uint8_t* codeStart = cb.start;
	LocationData loc = codeWriter.FindCodeLocation(currFuncIndex,pc-codeStart);
	errors.emplace_back(loc,type,msg);
}

void CodeInterpreter::EvalError(){
	PushError("EvalError","");
}

void CodeInterpreter::UndefinedError(SymbolName sym){
	std::stringstream msg{};
	msg << "Symbol '" << parser.GetSymbolString(sym);
	msg << "' is not defined!";
	PushError("UndefinedError",msg.str());
}

void CodeInterpreter::TypeError(SlangType found,SlangType expected){
	std::stringstream msg{};
	msg << "Expected type " << 
		TypeToString(expected) << " instead of type " << 
		TypeToString(found);
		
	PushError("TypeError",msg.str());
}

void CodeInterpreter::TypeError2(SlangType found,SlangType expected1,SlangType expected2){
	std::stringstream msg{};
	msg << "Expected type " << 
		TypeToString(expected1) << " or " << TypeToString(expected2) <<
		" instead of type " << TypeToString(found);
		
	PushError("TypeError",msg.str());
}

void CodeInterpreter::ArityError(size_t found,size_t expectMin,size_t expectMax){
	std::stringstream msg = {};
	const char* plural = (expectMin!=1) ? "s" : "";
	
	msg << "Procedure ";
	if (expectMin==expectMax){
		msg << "takes " << expectMin << " argument" << plural << ", was given " <<
			found;
	} else if (expectMax==VARIADIC_ARG_COUNT){
		msg << "takes at least " << expectMin << " argument" << plural << ", was given " <<
			found;
	} else {
		msg << "takes between " << expectMin << " and " << expectMax <<
			" arguments, was given " << found;
	}
		
	PushError("ArityError",msg.str());
}

void CodeInterpreter::IndexError(ssize_t desired,size_t len){
	std::stringstream msg = {};
	msg << "Attempted to access index " <<
		desired << " on an object with length " << len;
	
	PushError("IndexError",msg.str());
}

void CodeInterpreter::ListIndexError(ssize_t desired){
	std::stringstream msg = {};
	if (desired<0){
		msg << "Attempted to access index " <<
			desired << " of a list";
	} else {
		msg << "Attempted to access index " <<
			desired << " of a list with length <=" << desired;
	}
	
	PushError("IndexError",msg.str());
}

void CodeInterpreter::FileError(const std::string& msg){
	PushError("FileError",msg);
}

void CodeInterpreter::StreamError(const std::string& msg){
	PushError("StreamError",msg);
}

void CodeInterpreter::ExportError(SymbolName sym){
	std::stringstream ss = {};
	ss << "Symbol '" << parser.GetSymbolString(sym);
	ss << "' was exported twice!";
	PushError("ExportError",ss.str());
}

void CodeInterpreter::DotError(){
	PushError("TypeError","List instead of DottedList");
}

void CodeInterpreter::ZeroDivisionError(){
	PushError("ZeroDivisionError","Cannot divide by zero!");
}

void CodeInterpreter::AssertError(){
	PushError("AssertError","Assertion failed!");
}

void CodeInterpreter::UnwrapError(){
	PushError("UnwrapError","Tried to unwrap empty Maybe!");
}

inline bool GetTailOfList(SlangList* head,SlangList** into){
	while (head->right){
		if (!IsList(head->right)){
			return false;
		}
		head = (SlangList*)head->right;
	}
	*into = head;
	return true;
}

bool CodeFuncEval(CodeInterpreter* c){
	SlangHeader* codeObj = c->GetArg(0);
	if (!codeObj){
		c->Return(nullptr);
		return true;
	}
	
	if (!c->EvalCall(codeObj))
		return false;
	return true;
}

bool CodeFuncQuote(CodeInterpreter* c){
	SlangHeader* obj = c->GetArg(0);
	c->Return(c->Copy(obj));
	return true;
}

bool CodeFuncNot(CodeInterpreter* c){
	SlangHeader* obj = c->GetArg(0);
	if (ConvertToBool(obj)){
		c->Return(c->codeWriter.constFalseObj);
	} else {
		c->Return(c->codeWriter.constTrueObj);
	}
	return true;
}

bool CodeFuncIf(CodeInterpreter* c){
	SlangHeader* cond = c->GetArg(0);
	if (ConvertToBool(cond)){
		c->Return(c->GetArg(1));
	} else {
		if (c->GetArgCount()==2)
			c->Return(nullptr);
		else
			c->Return(c->GetArg(2));
	}
	return true;
}

bool CodeFuncApply(CodeInterpreter* c);

bool CodeFuncInc(CodeInterpreter* c){
	SlangHeader* val = c->GetArg(0);
	TYPE_CHECK_EXACT(val,SlangType::Int);
	
	SlangObj* res = c->alloc.MakeInt(((SlangObj*)val)->integer+1);
	c->Return((SlangHeader*)res);
	return true;
}

bool CodeFuncDec(CodeInterpreter* c){
	SlangHeader* val = c->GetArg(0);
	TYPE_CHECK_EXACT(val,SlangType::Int);
	
	SlangObj* res = c->alloc.MakeInt(((SlangObj*)val)->integer-1);
	c->Return((SlangHeader*)res);
	return true;
}

bool CodeFuncAdd(CodeInterpreter* c){
	size_t count = c->GetArgCount();
	if (!count){
		c->Return((SlangHeader*)c->alloc.MakeInt(0));
		return true;
	}
	SlangObj* res;
	bool isReal = false;
	if (GetType(c->GetArg(0))==SlangType::Real){
		res = c->alloc.MakeReal(0.0);
		isReal = true;
	} else {
		res = c->alloc.MakeInt(0);
	}
	for (size_t i=0;i<count;++i){
		SlangHeader* h = c->GetArg(i);
		TYPE_CHECK_NUMERIC(h);
		SlangObj* obj = (SlangObj*)h;
		
		if (h->type==SlangType::Real){
			if (!isReal){
				res = c->alloc.MakeReal(res->integer);
				obj = (SlangObj*)c->GetArg(i);
				isReal = true;
			}
			res->real += obj->real;
		} else {
			if (isReal){
				res->real += obj->integer;
			} else {
				res->integer += obj->integer;
			}
		}
	}
	c->Return((SlangHeader*)res);
	return true;
}

bool CodeFuncSub(CodeInterpreter* c){
	size_t count = c->GetArgCount();
	if (count==1){
		SlangHeader* first = c->GetArg(0);
		TYPE_CHECK_NUMERIC(first);
		if (first->type==SlangType::Real){
			c->Return((SlangHeader*)c->alloc.MakeReal(-((SlangObj*)first)->real));
		} else {
			c->Return((SlangHeader*)c->alloc.MakeInt(-((SlangObj*)first)->integer));
		}
		return true;
	}
	
	SlangObj* res;
	bool isReal = false;
	SlangHeader* first = c->GetArg(0);
	TYPE_CHECK_NUMERIC(first);
	
	SlangObj* firstObj = (SlangObj*)first;
	if (first->type==SlangType::Real){
		res = c->alloc.MakeReal(firstObj->real);
		isReal = true;
	} else {
		res = c->alloc.MakeInt(firstObj->integer);
	}
	for (size_t i=1;i<count;++i){
		SlangHeader* h = c->GetArg(i);
		TYPE_CHECK_NUMERIC(h);
		SlangObj* obj = (SlangObj*)h;
		
		if (h->type==SlangType::Real){
			if (!isReal){
				res = c->alloc.MakeReal(res->integer);
				obj = (SlangObj*)c->GetArg(i);
				isReal = true;
			}
			res->real -= obj->real;
		} else {
			if (isReal){
				res->real -= obj->integer;
			} else {
				res->integer -= obj->integer;
			}
		}
	}
	c->Return((SlangHeader*)res);
	return true;
}

bool CodeFuncMul(CodeInterpreter* c){
	size_t count = c->GetArgCount();
	if (!count){
		c->Return((SlangHeader*)c->alloc.MakeInt(1));
		return true;
	}
	SlangObj* res;
	bool isReal = false;
	if (GetType(c->GetArg(0))==SlangType::Real){
		res = c->alloc.MakeReal(1.0);
		isReal = true;
	} else {
		res = c->alloc.MakeInt(1);
	}
	for (size_t i=0;i<count;++i){
		SlangHeader* h = c->GetArg(i);
		TYPE_CHECK_NUMERIC(h);
		SlangObj* obj = (SlangObj*)h;
		
		if (h->type==SlangType::Real){
			if (!isReal){
				res = c->alloc.MakeReal(res->integer);
				obj = (SlangObj*)c->GetArg(i);
				isReal = true;
			}
			res->real *= obj->real;
		} else {
			if (isReal){
				res->real *= obj->integer;
			} else {
				res->integer *= obj->integer;
			}
		}
	}
	c->Return((SlangHeader*)res);
	return true;
}

bool CodeFuncDiv(CodeInterpreter* c){
	size_t count = c->GetArgCount();
	if (count==1){
		SlangHeader* first = c->GetArg(0);
		TYPE_CHECK_NUMERIC(first);
		if (first->type==SlangType::Real){
			double real = ((SlangObj*)first)->real;
			if (real==0.0){
				c->ZeroDivisionError();
				return false;
			}
			c->Return((SlangHeader*)c->alloc.MakeReal(1.0/real));
		} else {
			int64_t integer = ((SlangObj*)first)->integer;
			if (integer==0){
				c->ZeroDivisionError();
				return false;
			}
			c->Return((SlangHeader*)c->alloc.MakeInt(floordiv(1,integer)));
		}
		return true;
	}
	
	SlangObj* res;
	bool isReal = false;
	SlangHeader* first = c->GetArg(0);
	TYPE_CHECK_NUMERIC(first);
	SlangObj* firstObj = (SlangObj*)first;
	if (first->type==SlangType::Real){
		res = c->alloc.MakeReal(firstObj->real);
		isReal = true;
	} else {
		res = c->alloc.MakeInt(firstObj->integer);
	}
	for (size_t i=1;i<count;++i){
		SlangHeader* h = c->GetArg(i);
		TYPE_CHECK_NUMERIC(h);
		SlangObj* obj = (SlangObj*)h;
		
		if (h->type==SlangType::Real){
			if (!isReal){
				res = c->alloc.MakeReal(res->integer);
				obj = (SlangObj*)c->GetArg(i);
				isReal = true;
			}
			if (obj->real==0.0){
				c->ZeroDivisionError();
				return false;
			}
			res->real /= obj->real;
		} else {
			if (obj->integer==0){
				c->ZeroDivisionError();
				return false;
			}
			if (isReal){
				res->real /= obj->integer;
			} else {
				res->integer = floordiv(res->integer,obj->integer);
			}
		}
	}
	c->Return((SlangHeader*)res);
	return true;
}

bool CodeFuncMod(CodeInterpreter* c){
	SlangHeader* lObj = c->GetArg(0);
	SlangHeader* rObj = c->GetArg(1);
	
	SlangObj* l = (SlangObj*)lObj;
	SlangObj* r = (SlangObj*)rObj;
	
	if (GetType(lObj)==SlangType::Int){
		if (GetType(rObj)==SlangType::Int){
			if (r->integer==0){
				c->ZeroDivisionError();
				return false;
			}
			
			c->Return((SlangHeader*)c->alloc.MakeInt(
				floormod(l->integer,r->integer)
			));
			return true;
		} else if (GetType(rObj)==SlangType::Real){
			if (r->real==0.0){
				c->ZeroDivisionError();
				return false;
			}
			
			c->Return((SlangHeader*)c->alloc.MakeReal(
				floorfmod((double)l->integer,r->real)
			));
			return true;
		} else {
			c->TypeError2(GetType(rObj),SlangType::Int,SlangType::Real);
			return false;
		}
	} else if (GetType(lObj)==SlangType::Real){
		if (GetType(rObj)==SlangType::Int){
			if (r->integer==0){
				c->ZeroDivisionError();
				return false;
			}
		
			c->Return((SlangHeader*)c->alloc.MakeReal(
				floorfmod(l->real,(double)r->integer)
			));
			return true;
		} else if (GetType(rObj)==SlangType::Real){
			if (r->real==0.0){
				c->ZeroDivisionError();
				return false;
			}
		
			c->Return((SlangHeader*)c->alloc.MakeReal(
				floorfmod(l->real,r->real)
			));
			return true;
		} else {
			c->TypeError2(GetType(rObj),SlangType::Int,SlangType::Real);
			return false;
		}
	} else {
		c->TypeError2(GetType(lObj),SlangType::Int,SlangType::Real);
		return false;
	}
}

bool CodeFuncPow(CodeInterpreter* c){
	SlangHeader* lObj = c->GetArg(0);
	SlangHeader* rObj = c->GetArg(1);
	
	SlangObj* l = (SlangObj*)lObj;
	SlangObj* r = (SlangObj*)rObj;
	
	if (GetType(lObj)==SlangType::Int){
		if (GetType(rObj)==SlangType::Int){
			if (r->integer>=0){
				c->Return((SlangHeader*)c->alloc.MakeInt(intpow(l->integer,(uint64_t)r->integer)));
				return true;
			} else {
				if (l->integer==0){
					c->ZeroDivisionError();
					return false;
				}
				
				c->Return((SlangHeader*)c->alloc.MakeReal(pow((double)l->integer,(double)r->integer)));
				return true;
			}
		} else if (GetType(rObj)==SlangType::Real){
			if (l->integer==0&&r->real<0.0){
				c->ZeroDivisionError();
				return false;
			}
			c->Return((SlangHeader*)c->alloc.MakeReal(pow((double)l->integer,r->real)));
			return true;
		} else {
			c->TypeError2(GetType(rObj),SlangType::Int,SlangType::Real);
			return false;
		}
	} else if (GetType(lObj)==SlangType::Real){
		if (GetType(rObj)==SlangType::Int){
			if (l->real==0.0&&r->integer<0){
				c->ZeroDivisionError();
				return false;
			}
			c->Return((SlangHeader*)c->alloc.MakeReal(pow(l->real,(double)r->integer)));
			return true;
		} else if (GetType(rObj)==SlangType::Real){
			if (l->real==0.0&&r->real<0){
				c->ZeroDivisionError();
				return false;
			}
			c->Return((SlangHeader*)c->alloc.MakeReal(pow(l->real,r->real)));
			return true;
		} else {
			c->TypeError2(GetType(rObj),SlangType::Int,SlangType::Real);
			return false;
		}
	} else {
		c->TypeError2(GetType(lObj),SlangType::Int,SlangType::Real);
		return false;
	}
}

bool CodeFuncAbs(CodeInterpreter* c){
	SlangHeader* arg = c->GetArg(0);
	
	if (GetType(arg)==SlangType::Int){
		if (((SlangObj*)arg)->integer>=0)
			c->Return(arg);
		else
			c->Return(
				(SlangHeader*)c->alloc.MakeInt(-((SlangObj*)arg)->integer)
			);
		
		return true;
	} else if (GetType(arg)==SlangType::Real){
		if (((SlangObj*)arg)->real>0.0)
			c->Return(arg);
		else
			c->Return(
				(SlangHeader*)c->alloc.MakeReal(abs(((SlangObj*)arg)->real))
			);
			
		return true;
	} else {
		c->TypeError2(GetType(arg),SlangType::Int,SlangType::Real);
		return false;
	}
}

bool CodeFuncFloor(CodeInterpreter* c){
	SlangHeader* arg = c->GetArg(0);
	
	if (GetType(arg)==SlangType::Int){
		c->Return(arg);
		return true;
	} else if (GetType(arg)==SlangType::Real){
		c->Return(
			(SlangHeader*)c->alloc.MakeInt(floor(((SlangObj*)arg)->real))
		);
		return true;
	} else {
		c->TypeError2(GetType(arg),SlangType::Int,SlangType::Real);
		return false;
	}
}

bool CodeFuncCeil(CodeInterpreter* c){
	SlangHeader* arg = c->GetArg(0);
	
	if (GetType(arg)==SlangType::Int){
		c->Return(arg);
		return true;
	} else if (GetType(arg)==SlangType::Real){
		c->Return(
			(SlangHeader*)c->alloc.MakeInt(ceil(((SlangObj*)arg)->real))
		);
		return true;
	} else {
		c->TypeError2(GetType(arg),SlangType::Int,SlangType::Real);
		return false;
	}
}

bool CodeFuncMin(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	SlangHeader* first = c->GetArg(0);
	TYPE_CHECK_NUMERIC(first);
	SlangObj* res = (SlangObj*)first;
	
	for (size_t i=1;i<argCount;++i){
		SlangHeader* h = c->GetArg(i);
		SlangObj* hObj = (SlangObj*)h;
		if (res->header.type==SlangType::Int){
			if (GetType(h)==SlangType::Int){
				if (res->integer>hObj->integer){
					res = hObj;
				}
			} else if (GetType(h)==SlangType::Real){
				if ((double)res->integer>hObj->real){
					res = hObj;
				}
			} else {
				c->TypeError2(GetType(h),SlangType::Int,SlangType::Real);
				return false;
			}
		} else {
			if (GetType(h)==SlangType::Int){
				if (res->real>(double)hObj->integer){
					res = hObj;
				}
			} else if (GetType(h)==SlangType::Real){
				if (res->real>hObj->real){
					res = hObj;
				}
			} else {
				c->TypeError2(GetType(h),SlangType::Int,SlangType::Real);
				return false;
			}
		}
	}
	
	c->Return((SlangHeader*)res);
	return true;
}

bool CodeFuncMax(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	SlangHeader* first = c->GetArg(0);
	TYPE_CHECK_NUMERIC(first);
	SlangObj* res = (SlangObj*)first;
	
	for (size_t i=1;i<argCount;++i){
		SlangHeader* h = c->GetArg(i);
		SlangObj* hObj = (SlangObj*)h;
		if (res->header.type==SlangType::Int){
			if (GetType(h)==SlangType::Int){
				if (res->integer<hObj->integer){
					res = hObj;
				}
			} else if (GetType(h)==SlangType::Real){
				if ((double)res->integer<hObj->real){
					res = hObj;
				}
			} else {
				c->TypeError2(GetType(h),SlangType::Int,SlangType::Real);
				return false;
			}
		} else {
			if (GetType(h)==SlangType::Int){
				if (res->real<(double)hObj->integer){
					res = hObj;
				}
			} else if (GetType(h)==SlangType::Real){
				if (res->real<hObj->real){
					res = hObj;
				}
			} else {
				c->TypeError2(GetType(h),SlangType::Int,SlangType::Real);
				return false;
			}
		}
	}
	
	c->Return((SlangHeader*)res);
	return true;
}

bool CodeFuncBitAnd(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	
	uint64_t v = -1ULL;
	for (size_t i=0;i<argCount;++i){
		SlangHeader* arg = c->GetArg(i);
		TYPE_CHECK_EXACT(arg,SlangType::Int);
		v &= (uint64_t)((SlangObj*)arg)->integer;
	}
	
	c->Return((SlangHeader*)c->alloc.MakeInt(v));
	return true;
}

bool CodeFuncBitOr(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	
	uint64_t v = 0;
	for (size_t i=0;i<argCount;++i){
		SlangHeader* arg = c->GetArg(i);
		TYPE_CHECK_EXACT(arg,SlangType::Int);
		v |= (uint64_t)((SlangObj*)arg)->integer;
	}
	
	c->Return((SlangHeader*)c->alloc.MakeInt(v));
	return true;
}

bool CodeFuncBitXor(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	
	uint64_t v = 0;
	for (size_t i=0;i<argCount;++i){
		SlangHeader* arg = c->GetArg(i);
		TYPE_CHECK_EXACT(arg,SlangType::Int);
		v ^= (uint64_t)((SlangObj*)arg)->integer;
	}
	
	c->Return((SlangHeader*)c->alloc.MakeInt(v));
	return true;
}

bool CodeFuncBitNot(CodeInterpreter* c){
	SlangHeader* arg = c->GetArg(0);
	
	TYPE_CHECK_EXACT(arg,SlangType::Int);
	uint64_t val = ((SlangObj*)arg)->integer;
	c->Return((SlangHeader*)c->alloc.MakeInt(~val));
	return true;
}

bool CodeFuncBitLeftShift(CodeInterpreter* c){
	SlangHeader* arg = c->GetArg(0);
	SlangHeader* shift = c->GetArg(1);
	TYPE_CHECK_EXACT(arg,SlangType::Int);
	TYPE_CHECK_EXACT(shift,SlangType::Int);
	
	uint64_t val = ((SlangObj*)arg)->integer;
	int64_t s = ((SlangObj*)shift)->integer;
	if (s==0){
		c->Return(arg);
		return true;
	}
	if (s<=-64||s>=64){
		c->Return(c->codeWriter.constZeroObj);
		return true;
	}
	if (s<0){
		c->Return(
			(SlangHeader*)c->alloc.MakeInt(val>>(-s))
		);
		return true;
	}
	
	c->Return(
		(SlangHeader*)c->alloc.MakeInt(val<<s)
	);
	return true;
}

bool CodeFuncBitRightShift(CodeInterpreter* c){
	SlangHeader* arg = c->GetArg(0);
	SlangHeader* shift = c->GetArg(1);
	TYPE_CHECK_EXACT(arg,SlangType::Int);
	TYPE_CHECK_EXACT(shift,SlangType::Int);
	
	uint64_t val = ((SlangObj*)arg)->integer;
	int64_t s = ((SlangObj*)shift)->integer;
	if (s==0){
		c->Return(arg);
		return true;
	}
	if (s<=-64||s>=64){
		c->Return(c->codeWriter.constZeroObj);
		return true;
	}
	if (s<0){
		c->Return(
			(SlangHeader*)c->alloc.MakeInt(val<<(-s))
		);
		return true;
	}
	
	c->Return(
		(SlangHeader*)c->alloc.MakeInt(val>>s)
	);
	return true;
}

bool CodeFuncGT(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	if (argCount<2){
		c->Return(c->codeWriter.constTrueObj);
		return true;
	}
	
	SlangHeader* lObj;
	SlangHeader* rObj = c->GetArg(0);
	SlangObj* l;
	SlangObj* r;
	
	for (size_t i=1;i<argCount;++i){
		lObj = rObj;
		rObj = c->GetArg(i);
		l = (SlangObj*)lObj;
		r = (SlangObj*)rObj;
		if (GetType(lObj)==SlangType::Int){
			if (GetType(rObj)==SlangType::Int){
				if (!(l->integer>r->integer)){
					c->Return(c->codeWriter.constFalseObj);
					return true;
				}
			} else if (GetType(rObj)==SlangType::Real){
				if (!((double)l->integer>r->real)){
					c->Return(c->codeWriter.constFalseObj);
					return true;
				}
			} else {
				c->TypeError2(GetType(rObj),SlangType::Int,SlangType::Real);
				return false;
			}
		} else if (GetType(lObj)==SlangType::Real){
			if (GetType(rObj)==SlangType::Int){
				if (!(l->real>(double)r->integer)){
					c->Return(c->codeWriter.constFalseObj);
					return true;
				}
			} else if (GetType(rObj)==SlangType::Real){
				if (!(l->real>r->real)){
					c->Return(c->codeWriter.constFalseObj);
					return true;
				}
			} else {
				c->TypeError2(GetType(rObj),SlangType::Int,SlangType::Real);
				return false;
			}
		
		} else {
			c->TypeError2(GetType(lObj),SlangType::Int,SlangType::Real);
			return false;
		}
	}
	c->Return(c->codeWriter.constTrueObj);
	return true;
}

bool CodeFuncLT(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	if (argCount<2){
		c->Return(c->codeWriter.constTrueObj);
		return true;
	}
	
	SlangHeader* lObj;
	SlangHeader* rObj = c->GetArg(0);
	SlangObj* l;
	SlangObj* r;
	
	for (size_t i=1;i<argCount;++i){
		lObj = rObj;
		rObj = c->GetArg(i);
		l = (SlangObj*)lObj;
		r = (SlangObj*)rObj;
		if (GetType(lObj)==SlangType::Int){
			if (GetType(rObj)==SlangType::Int){
				if (!(l->integer<r->integer)){
					c->Return(c->codeWriter.constFalseObj);
					return true;
				}
			} else if (GetType(rObj)==SlangType::Real){
				if (!((double)l->integer<r->real)){
					c->Return(c->codeWriter.constFalseObj);
					return true;
				}
			} else {
				c->TypeError2(GetType(rObj),SlangType::Int,SlangType::Real);
				return false;
			}
		} else if (GetType(lObj)==SlangType::Real){
			if (GetType(rObj)==SlangType::Int){
				if (!(l->real<(double)r->integer)){
					c->Return(c->codeWriter.constFalseObj);
					return true;
				}
			} else if (GetType(rObj)==SlangType::Real){
				if (!(l->real<r->real)){
					c->Return(c->codeWriter.constFalseObj);
					return true;
				}
			} else {
				c->TypeError2(GetType(rObj),SlangType::Int,SlangType::Real);
				return false;
			}
		
		} else {
			c->TypeError2(GetType(lObj),SlangType::Int,SlangType::Real);
			return false;
		}
	}
	c->Return(c->codeWriter.constTrueObj);
	return true;
}

bool CodeFuncGTE(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	if (argCount<2){
		c->Return(c->codeWriter.constTrueObj);
		return true;
	}
	
	SlangHeader* lObj;
	SlangHeader* rObj = c->GetArg(0);
	SlangObj* l;
	SlangObj* r;
	
	for (size_t i=1;i<argCount;++i){
		lObj = rObj;
		rObj = c->GetArg(i);
		l = (SlangObj*)lObj;
		r = (SlangObj*)rObj;
		if (GetType(lObj)==SlangType::Int){
			if (GetType(rObj)==SlangType::Int){
				if (!(l->integer>=r->integer)){
					c->Return(c->codeWriter.constFalseObj);
					return true;
				}
			} else if (GetType(rObj)==SlangType::Real){
				if (!((double)l->integer>=r->real)){
					c->Return(c->codeWriter.constFalseObj);
					return true;
				}
			} else {
				c->TypeError2(GetType(rObj),SlangType::Int,SlangType::Real);
				return false;
			}
		} else if (GetType(lObj)==SlangType::Real){
			if (GetType(rObj)==SlangType::Int){
				if (!(l->real>=(double)r->integer)){
					c->Return(c->codeWriter.constFalseObj);
					return true;
				}
			} else if (GetType(rObj)==SlangType::Real){
				if (!(l->real>=r->real)){
					c->Return(c->codeWriter.constFalseObj);
					return true;
				}
			} else {
				c->TypeError2(GetType(rObj),SlangType::Int,SlangType::Real);
				return false;
			}
		
		} else {
			c->TypeError2(GetType(lObj),SlangType::Int,SlangType::Real);
			return false;
		}
	}
	c->Return(c->codeWriter.constTrueObj);
	return true;
}

bool CodeFuncLTE(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	if (argCount<2){
		c->Return(c->codeWriter.constTrueObj);
		return true;
	}
	
	SlangHeader* lObj;
	SlangHeader* rObj = c->GetArg(0);
	SlangObj* l;
	SlangObj* r;
	
	for (size_t i=1;i<argCount;++i){
		lObj = rObj;
		rObj = c->GetArg(i);
		l = (SlangObj*)lObj;
		r = (SlangObj*)rObj;
		if (GetType(lObj)==SlangType::Int){
			if (GetType(rObj)==SlangType::Int){
				if (!(l->integer<=r->integer)){
					c->Return(c->codeWriter.constFalseObj);
					return true;
				}
			} else if (GetType(rObj)==SlangType::Real){
				if (!((double)l->integer<=r->real)){
					c->Return(c->codeWriter.constFalseObj);
					return true;
				}
			} else {
				c->TypeError2(GetType(rObj),SlangType::Int,SlangType::Real);
				return false;
			}
		} else if (GetType(lObj)==SlangType::Real){
			if (GetType(rObj)==SlangType::Int){
				if (!(l->real<=(double)r->integer)){
					c->Return(c->codeWriter.constFalseObj);
					return true;
				}
			} else if (GetType(rObj)==SlangType::Real){
				if (!(l->real<=r->real)){
					c->Return(c->codeWriter.constFalseObj);
					return true;
				}
			} else {
				c->TypeError2(GetType(rObj),SlangType::Int,SlangType::Real);
				return false;
			}
		
		} else {
			c->TypeError2(GetType(lObj),SlangType::Int,SlangType::Real);
			return false;
		}
	}
	c->Return(c->codeWriter.constTrueObj);
	return true;
}

bool CodeFuncPrint(CodeInterpreter* c){
	size_t count = c->GetArgCount();
	
	for (size_t i=0;i<count;++i){
		SlangHeader* a = c->GetArg(i);
		if (!a){
			std::cout << "()";
		} else {
			std::cout << *a;
		}
		if (i!=count-1) std::cout << " ";
	}
	std::cout << '\n';
	c->Return(nullptr);
	return true;
}

bool CodeFuncInput(CodeInterpreter* c){
	c->Return(c->SlangInputFromFile(stdin));
	return true;
}

bool CodeFuncOutput(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	
	SlangHeader* printObj;
	for (size_t i=0;i<argCount;++i){
		printObj = c->GetArg(i);
		
		if (!c->SlangOutputToFile(stdout,printObj))
			return false;
	}
	
	c->Return(nullptr);
	return true;
}

bool CodeFuncAssert(CodeInterpreter* c){
	bool t = ConvertToBool(c->GetArg(0));
	if (!t){
		c->AssertError();
		return false;
	}
	c->Return(c->GetArg(0));
	return true;
}

bool CodeFuncEq(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	for (size_t i=1;i<argCount;++i){
		if (!EqualObjs(c->GetArg(0),c->GetArg(i))){
			c->Return(c->codeWriter.constFalseObj);
			return true;
		}
	}
	c->Return(c->codeWriter.constTrueObj);
	return true;
}

bool CodeFuncNEq(CodeInterpreter* c){
	if (EqualObjs(c->GetArg(0),c->GetArg(1)))
		c->Return(c->codeWriter.constFalseObj);
	else
		c->Return(c->codeWriter.constTrueObj);
	return true;
}

bool CodeFuncIs(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	
	for (size_t i=1;i<argCount;++i){
		if (!IdenticalObjs(c->GetArg(0),c->GetArg(i))){
			c->Return(c->codeWriter.constFalseObj);
			return true;
		}
	}
	c->Return(c->codeWriter.constTrueObj);
	return true;
}

bool CodeFuncLen(CodeInterpreter* c){
	SlangHeader* obj = c->GetArg(0);
	if (!obj){
		c->Return(c->codeWriter.constZeroObj);
		return true;
	}
	
	SlangType t = GetType(obj);
	switch (t){
		case SlangType::Vector: {
			SlangVec* v = (SlangVec*)obj;
			if (!v->storage)
				c->Return(c->codeWriter.constZeroObj);
			else
				c->Return((SlangHeader*)c->alloc.MakeInt(v->storage->size));
			return true;
		}
		case SlangType::Dict: {
			SlangDict* d = (SlangDict*)obj;
			if (!d->table)
				c->Return(c->codeWriter.constZeroObj);
			else
				c->Return((SlangHeader*)c->alloc.MakeInt(d->table->size));
			return true;
		}
		case SlangType::String: {
			SlangStr* str = (SlangStr*)obj;
			if (!str->storage)
				c->Return(c->codeWriter.constZeroObj);
			else
				c->Return((SlangHeader*)c->alloc.MakeInt(str->storage->size));
			return true;
		}
		case SlangType::Maybe: {
			if (obj->flags & FLAG_MAYBE_OCCUPIED)
				c->Return(c->codeWriter.constOneObj);
			else
				c->Return(c->codeWriter.constZeroObj);
			return true;
		}
		case SlangType::List: {
			c->Return((SlangHeader*)c->alloc.MakeInt(GetArgCount((SlangList*)obj)));
			return true;
		}
		default:
			c->TypeError(t,SlangType::Vector);
			return false;
	}
}

bool CodeFuncParse(CodeInterpreter* c){
	SlangHeader* strObj = c->GetArg(0);
	TYPE_CHECK_EXACT(strObj,SlangType::String);
	
	SlangHeader* parsed;
	if (!c->ParseSlangString(*(SlangStr*)strObj,&parsed)){
		c->PushError("ParseError","Could not parse!");
		return false;
	}
	c->Return(parsed);
	return true;
}

bool CodeFuncUnwrap(CodeInterpreter* c){
	SlangHeader* obj = c->GetArg(0);
	if (GetType(obj)!=SlangType::Maybe){
		c->TypeError(GetType(obj),SlangType::Maybe);
		return false;
	}
	
	if (!(obj->flags & FLAG_MAYBE_OCCUPIED)){
		c->UnwrapError();
		return false;
	}
	
	c->Return(((SlangObj*)obj)->maybe);
	return true;
}

bool CodeFuncEmpty(CodeInterpreter* c){
	SlangHeader* obj = c->GetArg(0);
	SlangType t = GetType(obj);
	switch (t){
		case SlangType::Vector: {
			SlangVec* v = (SlangVec*)obj;
			if (!v->storage || v->storage->size==0)
				c->Return(c->codeWriter.constTrueObj);
			else
				c->Return(c->codeWriter.constFalseObj);
			return true;
		}
		case SlangType::Dict: {
			SlangDict* dict = (SlangDict*)obj;
			if (!dict->table || dict->table->size==0)
				c->Return(c->codeWriter.constTrueObj);
			else
				c->Return(c->codeWriter.constFalseObj);
			return true;
		}
		case SlangType::String: {
			SlangStr* str = (SlangStr*)obj;
			if (!str->storage || str->storage->size==0)
				c->Return(c->codeWriter.constTrueObj);
			else
				c->Return(c->codeWriter.constFalseObj);
			return true;
		}
		case SlangType::Maybe: {
			if (obj->flags & FLAG_MAYBE_OCCUPIED)
				c->Return(c->codeWriter.constFalseObj);
			else
				c->Return(c->codeWriter.constTrueObj);
			return true;
		}
		case SlangType::NullType: {
			c->Return(c->codeWriter.constTrueObj);
			return true;
		}
		default:
			c->TypeError2(t,SlangType::Maybe,SlangType::Storage);
			return false;
	}
}

bool CodeFuncAnd(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	SlangHeader* obj;
	for (size_t i=0;i<argCount;++i){
		obj = c->GetArg(i);
		if (!ConvertToBool(obj)){
			c->Return(obj);
			return true;
		}
	}
	if (!argCount)
		c->Return(c->codeWriter.constTrueObj);
	else
		c->Return(obj);
	return true;
}

bool CodeFuncOr(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	SlangHeader* obj;
	for (size_t i=0;i<argCount;++i){
		obj = c->GetArg(i);
		if (ConvertToBool(obj)){
			c->Return(obj);
			return true;
		}
	}
	if (!argCount)
		c->Return(c->codeWriter.constFalseObj);
	else
		c->Return(obj);
	return true;
}

bool CodeFuncMap(CodeInterpreter* c);

bool CodeFuncPair(CodeInterpreter* c){
	SlangList* l = c->alloc.AllocateList();
	l->left = c->GetArg(0);
	l->right = c->GetArg(1);
	c->Return((SlangHeader*)l);
	return true;
}

bool CodeFuncLeft(CodeInterpreter* c){
	SlangHeader* obj = c->GetArg(0);
	if (GetType(obj)!=SlangType::List){
		c->TypeError(GetType(obj),SlangType::List);
		return false;
	}
	c->Return(((SlangList*)obj)->left);
	return true;
}

bool CodeFuncRight(CodeInterpreter* c){
	SlangHeader* obj = c->GetArg(0);
	if (GetType(obj)!=SlangType::List){
		c->TypeError(GetType(obj),SlangType::List);
		return false;
	}
	c->Return(((SlangList*)obj)->right);
	return true;
}

bool CodeFuncSetLeft(CodeInterpreter* c){
	SlangHeader* obj = c->GetArg(0);
	if (GetType(obj)!=SlangType::List){
		c->TypeError(GetType(obj),SlangType::List);
		return false;
	}
	if (!c->arena->InCurrSet((uint8_t*)obj)){
		c->PushError("SetError","Cannot set left of const data!");
		return false;
	}
	
	SlangHeader* val = c->GetArg(1);
	((SlangList*)obj)->left = val;
	c->Return(nullptr);
	return true;
}

bool CodeFuncSetRight(CodeInterpreter* c){
	SlangHeader* obj = c->GetArg(0);
	if (GetType(obj)!=SlangType::List){
		c->TypeError(GetType(obj),SlangType::List);
		return false;
	}
	if (!c->arena->InCurrSet((uint8_t*)obj)){
		c->PushError("SetError","Cannot set right of const data!");
		return false;
	}
	
	SlangHeader* val = c->GetArg(1);
	((SlangList*)obj)->right = val;
	c->Return(nullptr);
	return true;
}

bool CodeFuncList(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	if (!argCount){
		c->Return(nullptr);
		return true;
	}
	SlangList* head = c->alloc.AllocateList();
	head->left = nullptr;
	head->right = nullptr;
	size_t headIndex = c->argStack.size;
	c->PushArg((SlangHeader*)head);
	size_t currIndex = c->argStack.size;
	SlangList* curr = head;
	c->PushArg((SlangHeader*)curr);
	for (size_t i=0;i<argCount-1;++i){
		SlangList* newL = c->alloc.AllocateList();
		newL->left = nullptr;
		newL->right = nullptr;
		curr = (SlangList*)c->argStack.data[currIndex];
		curr->left = c->GetArg(i);
		curr->right = (SlangHeader*)newL;
		c->argStack.data[currIndex] = ((SlangList*)c->argStack.data[currIndex])->right;
	}
	((SlangList*)c->argStack.data[currIndex])->left = c->GetArg(argCount-1);
	c->Return(c->argStack.data[headIndex]);
	return true;
}

bool CodeFuncListGet(CodeInterpreter* c){
	SlangHeader* listObj = c->GetArg(0);
	SlangHeader* indexObj = c->GetArg(1);
	
	TYPE_CHECK_EXACT(indexObj,SlangType::Int);
	if (!listObj){
		c->ListIndexError(((SlangObj*)indexObj)->integer);
		return false;
	}
	
	TYPE_CHECK_EXACT(listObj,SlangType::List);
	
	int64_t v = ((SlangObj*)indexObj)->integer;
	if (v<0){
		c->ListIndexError(v);
		return false;
	}
	SlangList* list = (SlangList*)listObj;
	
	while (v){
		--v;
		list = (SlangList*)list->right;
		
		if (GetType((SlangHeader*)list)!=SlangType::List){
			c->ListIndexError(((SlangObj*)indexObj)->integer);
			return false;
		}
	}
	c->Return(list->left);
	return true;
}

bool CodeFuncListSet(CodeInterpreter* c){
	SlangHeader* listObj = c->GetArg(0);
	SlangHeader* indexObj = c->GetArg(1);
	SlangHeader* item = c->GetArg(2);
	
	TYPE_CHECK_EXACT(indexObj,SlangType::Int);
	if (!listObj){
		c->ListIndexError(((SlangObj*)indexObj)->integer);
		return false;
	}
	
	TYPE_CHECK_EXACT(listObj,SlangType::List);
	
	int64_t v = ((SlangObj*)indexObj)->integer;
	if (v<0){
		c->ListIndexError(v);
		return false;
	}
	SlangList* list = (SlangList*)listObj;
	
	while (v){
		--v;
		list = (SlangList*)list->right;
		
		if (GetType((SlangHeader*)list)!=SlangType::List){
			c->ListIndexError(((SlangObj*)indexObj)->integer);
			return false;
		}
	}
	
	if (!c->arena->InCurrSet((uint8_t*)list)){
		c->PushError("SetError","Cannot set left of const data!");
		return false;
	}
	list->left = item;
	c->Return(nullptr);
	return true;
}

bool CodeFuncListConcat(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	if (!argCount){
		c->Return(nullptr);
		return true;
	}
	
	if (argCount==1){
		c->Return(c->GetArg(0));
		return true;
	}
	
	
	SlangList* list = nullptr;
	
	for (size_t i=0;i<argCount-1;++i){
		SlangHeader* arg = c->GetArg(i);
		if (GetType(arg)!=SlangType::List){
			c->TypeError(GetType(arg),SlangType::List);
			return false;
		}
		if (list){
			c->PushArg((SlangHeader*)list);
			SlangList* copy = c->CopyList((SlangList*)arg);
			list = (SlangList*)c->PopArg();
			
			list->right = (SlangHeader*)copy;
		} else {
			list = c->CopyList((SlangList*)arg);
			c->PushArg((SlangHeader*)list);
		}
		
		if (!GetTailOfList(list,&list)){
			c->DotError();
			return false;
		}
	}
	
	SlangHeader* last = c->GetArg(argCount-1);
	list->right = last;
	c->Return(c->PopArg());
	return true;
}

bool CodeFuncOutputTo(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	SlangHeader* streamObj = c->GetArg(0);
	
	TYPE_CHECK_EXACT(streamObj,SlangType::OutputStream);
	
	SlangStream* stream  = (SlangStream*)streamObj;
	if (streamObj->isFile){
		if (!stream->file){
			c->FileError("Cannot write to a closed file!");
			return false;
		}
		for (size_t i=1;i<argCount;++i){
			stream = (SlangStream*)c->GetArg(0);
			SlangHeader* obj = c->GetArg(i);
			if (!c->SlangOutputToFile(stream->file,obj)){
				return false;
			}
		}
	} else {
		for (size_t i=1;i<argCount;++i){
			stream = (SlangStream*)c->GetArg(0);
			SlangHeader* obj = c->GetArg(i);
			if (!c->SlangOutputToString(stream,obj)){
				return false;
			}
		}
	}
	
	c->Return(nullptr);
	return true;
}

bool CodeFuncInputFrom(CodeInterpreter* c){
	SlangHeader* streamObj = c->GetArg(0);
	TYPE_CHECK_EXACT(streamObj,SlangType::InputStream);
	
	SlangStream* stream = (SlangStream*)streamObj;
	if (streamObj->isFile){
		if (!stream->file){
			c->FileError("Cannot read from a closed file!");
			return false;
		}
		c->Return(c->SlangInputFromFile(stream->file));
	} else {
		c->Return(c->SlangInputFromString(stream));
	}
	return true;
}

bool CodeFuncVec(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	SlangVec* vec = c->alloc.AllocateVec(argCount);
	
	if (argCount){
		size_t base = c->stack.Back().base;
		memcpy(vec->storage->objs,&c->argStack.data[base],argCount*sizeof(SlangHeader*));
	}
	
	c->Return((SlangHeader*)vec);
	return true;
}

bool CodeFuncVecAlloc(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	SlangHeader* sizeObj = c->GetArg(0);
	if (GetType(sizeObj)!=SlangType::Int){
		c->TypeError(GetType(sizeObj),SlangType::Int);
		return false;
	}
	size_t vecSize = ((SlangObj*)sizeObj)->integer;
	SlangVec* vec = c->alloc.AllocateVec(vecSize);
	SlangHeader* fill = nullptr;
	if (argCount==2){
		fill = c->GetArg(1);
	}
	if (!fill){
		memset(vec->storage->objs,0,vecSize*sizeof(SlangHeader*));
	} else {
		for (size_t i=0;i<vecSize;++i){
			vec->storage->objs[i] = fill;
		}
	}
	
	c->Return((SlangHeader*)vec);
	return true;
}

bool CodeFuncVecGet(CodeInterpreter* c){
	SlangHeader* vecObj = c->GetArg(0);
	if (GetType(vecObj)!=SlangType::Vector){
		c->TypeError(GetType(vecObj),SlangType::Vector);
		return false;
	}
	
	SlangVec* vec = (SlangVec*)vecObj;
	SlangHeader* indexObj = c->GetArg(1);
	if (GetType(indexObj)!=SlangType::Int){
		c->TypeError(GetType(indexObj),SlangType::Int);
		return false;
	}
	
	int64_t index = ((SlangObj*)indexObj)->integer;
	if (!vec->storage){
		c->IndexError(index,0);
		return false;
	}
	
	if (index<-(int64_t)vec->storage->size||index>=(int64_t)vec->storage->size){
		c->IndexError(index,vec->storage->size);
		return false;
	}
	
	if (index<0){
		index += vec->storage->size;
	}
	
	c->Return(vec->storage->objs[index]);
	return true;
}

bool CodeFuncVecSet(CodeInterpreter* c){
	SlangHeader* vecObj = c->GetArg(0);
	if (GetType(vecObj)!=SlangType::Vector){
		c->TypeError(GetType(vecObj),SlangType::Vector);
		return false;
	}
	
	SlangVec* vec = (SlangVec*)vecObj;
	SlangHeader* indexObj = c->GetArg(1);
	if (GetType(indexObj)!=SlangType::Int){
		c->TypeError(GetType(indexObj),SlangType::Int);
		return false;
	}
	
	int64_t index = ((SlangObj*)indexObj)->integer;
	if (!vec->storage){
		c->IndexError(index,0);
		return false;
	}
	
	if (index<-(int64_t)vec->storage->size||index>=(int64_t)vec->storage->size){
		c->IndexError(index,vec->storage->size);
		return false;
	}
	
	if (!c->arena->InCurrSet((uint8_t*)vec)){
		c->PushError("SetError","Cannot vec-set! const vector!");
		return false;
	}
	
	if (index<0){
		index += vec->storage->size;
	}
	
	SlangHeader* setObj = c->GetArg(2);
	
	vec->storage->objs[index] = setObj;
	
	c->Return(nullptr);
	return true;
}

bool CodeFuncVecApp(CodeInterpreter* c){
	SlangHeader* vecObj = c->GetArg(0);
	if (GetType(vecObj)!=SlangType::Vector){
		c->TypeError(GetType(vecObj),SlangType::Vector);
		return false;
	}
	
	SlangVec* vec = (SlangVec*)vecObj;
	if (!c->arena->InCurrSet((uint8_t*)vec)){
		c->PushError("SetError","Cannot vec-app! const vector!");
		return false;
	}
	
	if (vec->storage&&vec->storage->size<vec->storage->capacity){
		vec->storage->objs[vec->storage->size++] = c->GetArg(1);
		c->Return(nullptr);
		return true;
	}
	
	SlangStorage* newStorage;
	if (!vec->storage){
		newStorage = c->alloc.AllocateStorage(2,sizeof(SlangHeader*));
		newStorage->size = 0;
	} else {
		size_t oldSize = vec->storage->size;
		newStorage = c->alloc.AllocateStorage(3*vec->storage->capacity/2,sizeof(SlangHeader*));
		newStorage->size = oldSize;
	}
	
	vec = (SlangVec*)c->GetArg(0);
	memcpy(newStorage->objs,vec->storage->objs,vec->storage->size*sizeof(SlangHeader*));
	vec->storage = newStorage;
	vec->storage->objs[vec->storage->size++] = c->GetArg(1);
	c->Return(nullptr);
	return true;
}

bool CodeFuncVecPop(CodeInterpreter* c){
	SlangHeader* vecObj = c->GetArg(0);
	if (GetType(vecObj)!=SlangType::Vector){
		c->TypeError(GetType(vecObj),SlangType::Vector);
		return false;
	}
	
	SlangVec* vec = (SlangVec*)vecObj;
	if (!c->arena->InCurrSet((uint8_t*)vec)){
		c->PushError("SetError","Cannot vec-pop! const vector!");
		return false;
	}
	
	if (!vec->storage || vec->storage->size==0){
		c->PushError("IndexError","Cannot pop from empty vector!");
		return false;
	}
	
	SlangHeader* obj = vec->storage->objs[vec->storage->size-1];
	--vec->storage->size;
	c->Return(obj);
	return true;
}

bool CodeFuncDict(CodeInterpreter* c){
	SlangDict* dict = c->alloc.AllocateDict();
	c->Return((SlangHeader*)dict);
	return true;
}

bool CodeFuncDictGet(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	SlangHeader* dictObj = c->GetArg(0);
	SlangHeader* keyObj = c->GetArg(1);
	TYPE_CHECK_EXACT(dictObj,SlangType::Dict);
	SlangDict* dict = (SlangDict*)dictObj;
	
	if (!IsHashable(keyObj)){
		c->PushError("HashError","Unhashable type!");
		return false;
	}
	
	SlangHeader* val;
	if (!dict->LookupKey(keyObj,&val)){
		if (argCount==3){
			c->Return(c->GetArg(2));
			return true;
		}
		std::stringstream ss{};
		ss << "Key '";
		if (keyObj)
			ss << *keyObj;
		else
			ss << "()";
		ss << "' is not in the dict!";
		c->PushError("KeyError",ss.str());
		return false;
	}
	c->Return(val);
	return true;
}

bool CodeFuncDictSet(CodeInterpreter* c){
	SlangHeader* dictObj = c->GetArg(0);
	SlangHeader* keyObj = c->GetArg(1);
	SlangHeader* valObj = c->GetArg(2);
	TYPE_CHECK_EXACT(dictObj,SlangType::Dict);
	SlangDict* dict = (SlangDict*)dictObj;
	
	if (!IsHashable(keyObj)){
		c->PushError("HashError","Unhashable type!");
		return false;
	}
	
	c->DictInsert(dict,keyObj,valObj);
	c->Return(nullptr);
	return true;
}

bool CodeFuncDictPop(CodeInterpreter* c){
	SlangHeader* dictObj = c->GetArg(0);
	SlangHeader* keyObj = c->GetArg(1);
	TYPE_CHECK_EXACT(dictObj,SlangType::Dict);
	SlangDict* dict = (SlangDict*)dictObj;
	
	if (!IsHashable(keyObj)){
		c->PushError("HashError","Unhashable type!");
		return false;
	}
	
	SlangHeader* val;
	if (!dict->PopKey(keyObj,&val)){
		std::stringstream ss{};
		ss << "Key '";
		if (keyObj)
			ss << *keyObj;
		else
			ss << "()";
		ss << "' is not in the dict!";
		c->PushError("KeyError",ss.str());
		
		return false;
	}
	c->Return(val);
	return true;
}

bool CodeFuncStrGet(CodeInterpreter* c){
	SlangHeader* strObj = c->GetArg(0);
	SlangHeader* indexObj = c->GetArg(1);
	
	TYPE_CHECK_EXACT(strObj,SlangType::String);
	TYPE_CHECK_EXACT(indexObj,SlangType::Int);
	
	SlangStr* str = (SlangStr*)strObj;
	int64_t index = ((SlangObj*)indexObj)->integer;
	
	if (!str->storage){
		c->IndexError(index,0);
		return false;
	}
	
	if (index<-(int64_t)str->storage->size||index>=(int64_t)str->storage->size){
		c->IndexError(index,str->storage->size);
		return false;
	}
	
	if (index<0){
		index += str->storage->size;
	}
	
	uint8_t ch = str->storage->data[index];
	SlangStr* newStr = c->alloc.AllocateStr(1);
	newStr->storage->data[0] = ch;
	assert(newStr->storage->size==1);
	c->Return((SlangHeader*)newStr);
	return true;
}

bool CodeFuncStrSet(CodeInterpreter* c){
	SlangHeader* strObj = c->GetArg(0);
	SlangHeader* indexObj = c->GetArg(1);
	SlangHeader* setStrObj = c->GetArg(2);
	
	TYPE_CHECK_EXACT(strObj,SlangType::String);
	TYPE_CHECK_EXACT(indexObj,SlangType::Int);
	TYPE_CHECK_EXACT(setStrObj,SlangType::String);
	
	SlangStr* str = (SlangStr*)strObj;
	int64_t index = ((SlangObj*)indexObj)->integer;
	
	if (!str->storage){
		c->IndexError(index,0);
		return false;
	}
	
	if (index<-(int64_t)str->storage->size||index>=(int64_t)str->storage->size){
		c->IndexError(index,str->storage->size);
		return false;
	}
	
	if (index<0){
		index += str->storage->size;
	}
	
	SlangStr* setStr = (SlangStr*)setStrObj;
	if (!setStr->storage||setStr->storage->size!=1){
		std::stringstream ss{};
		ss << "Expected char, not string of size ";
		ss << ((setStr->storage) ? setStr->storage->size : 0);
		c->PushError("StrError",ss.str());
		return false;
	}
	
	str->storage->data[index] = setStr->storage->data[0];
	c->Return(nullptr);
	return true;
}

bool CodeFuncStrApp(CodeInterpreter* c){
	SlangHeader* strObj = c->GetArg(0);
	SlangHeader* setStrObj = c->GetArg(1);
	
	TYPE_CHECK_EXACT(strObj,SlangType::String);
	TYPE_CHECK_EXACT(setStrObj,SlangType::String);
	
	SlangStr* str = (SlangStr*)strObj;
	SlangStr* setStr = (SlangStr*)setStrObj;
	
	if (!setStr->storage||setStr->storage->size==0){
		return true;
	}
	
	size_t addSize = setStr->storage->size;
	
	if (!str->storage){
		str = c->ReallocateStr(str,addSize);
		setStr = (SlangStr*)c->GetArg(1);
		memcpy(str->storage->data,setStr->storage->data,addSize);
		str->storage->size = addSize;
		return true;
	}
	
	size_t start = str->storage->size;
	if (str->storage->size+addSize<str->storage->capacity){
		str = c->ReallocateStr(str,(str->storage->size+addSize)*3/2);
		setStr = (SlangStr*)c->GetArg(1);
	}
	memcpy(str->storage->data+start,setStr->storage->data,addSize);
	str->storage->size += addSize;
	c->Return(nullptr);
	return true;
}

bool CodeFuncStrPop(CodeInterpreter* c){
	SlangHeader* strObj = c->GetArg(0);
	TYPE_CHECK_EXACT(strObj,SlangType::String);
	SlangStr* str = (SlangStr*)strObj;
	
	if (!str->storage||str->storage->size==0){
		c->PushError("StrError","Cannot pop from empty string!");
		return false;
	}
	
	uint8_t ch = str->storage->data[--str->storage->size];
	SlangStr* newStr = c->alloc.AllocateStr(1);
	newStr->storage->size = 1;
	newStr->storage->data[0] = ch;
	c->Return((SlangHeader*)newStr);
	return true;
}

bool CodeFuncStrSplit(CodeInterpreter* c){
	static char splitterArray[256];
	
	SlangHeader* strObj = c->GetArg(0);
	TYPE_CHECK_EXACT(strObj,SlangType::String);
	SlangStr* str = (SlangStr*)strObj;
	size_t splitterCount = c->GetArgCount()-1;
	
	for (size_t i=0;i<splitterCount;++i){
		SlangHeader* splitArg = c->GetArg(1+i);
		TYPE_CHECK_EXACT(splitArg,SlangType::String);
		SlangStr* splitChar = (SlangStr*)splitArg;
		if (GetStorageSize(splitChar->storage)!=1){
			std::stringstream ss{};
			ss << "Expected char, not string of size ";
			ss << ((splitChar->storage) ? splitChar->storage->size : 0);
			c->PushError("StrError",ss.str());
			return false;
		}
		splitterArray[i] = splitChar->storage->data[0];
	}
	if (!str->storage){
		c->Return(nullptr);
		return true;
	}
	
	SlangList* listHead = c->alloc.AllocateList();
	listHead->right = nullptr;
	listHead->left = nullptr;
	size_t headIndex = c->argStack.size;
	c->PushArg((SlangHeader*)listHead);
	SlangList* listIt = listHead;
	size_t itIndex = c->argStack.size;
	c->PushArg((SlangHeader*)listIt);
	size_t grabStart = 0;
	size_t strPos = 0;
	str = (SlangStr*)c->GetArg(0);
	while (true){
		if (strPos>=str->storage->size){
			break;
		}
		char ch = str->storage->data[strPos];
		for (size_t i=0;i<splitterCount;++i){
			if (ch==splitterArray[i]){
				SlangStr* piece = c->alloc.AllocateStr(strPos-grabStart);
				c->PushArg((SlangHeader*)piece);
				SlangList* newList = c->alloc.AllocateList();
				piece = (SlangStr*)c->PopArg();
				str = (SlangStr*)c->GetArg(0);
				if (strPos!=grabStart){
					memcpy(piece->storage->data,str->storage->data+grabStart,strPos-grabStart);
				}
				newList->left = (SlangHeader*)piece;
				newList->right = nullptr;
				if (grabStart==0){
					c->argStack.data[headIndex] = (SlangHeader*)newList;
					c->argStack.data[itIndex] = (SlangHeader*)newList;
				} else {
					listIt = (SlangList*)c->argStack.data[itIndex];
					listIt->right = (SlangHeader*)newList;
					c->argStack.data[itIndex] = (SlangHeader*)newList;
				}
				
				grabStart = strPos+1;
				break;
			}
		}
		++strPos;
	}
	
	
	SlangStr* finalPiece = c->alloc.AllocateStr(strPos-grabStart);
	c->PushArg((SlangHeader*)finalPiece);
	SlangList* newList = c->alloc.AllocateList();
	finalPiece = (SlangStr*)c->PopArg();
	str = (SlangStr*)c->GetArg(0);
	if (strPos!=grabStart){
		memcpy(finalPiece->storage->data,str->storage->data+grabStart,strPos-grabStart);
	}
	newList->left = (SlangHeader*)finalPiece;
	newList->right = nullptr;
	if (grabStart==0){
		c->argStack.data[headIndex] = (SlangHeader*)newList;
	} else {
		listIt = (SlangList*)c->argStack.data[itIndex];
		listIt->right = (SlangHeader*)newList;
	}
	
	listHead = (SlangList*)c->argStack.data[headIndex];
	c->Return((SlangHeader*)listHead);
	return true;
}

bool CodeFuncStrJoin(CodeInterpreter* c){
	SlangHeader* chObj = c->GetArg(0);
	TYPE_CHECK_EXACT(chObj,SlangType::String);
	SlangStr* joinStr = (SlangStr*)chObj;
	size_t joinStrSize = GetStorageSize(joinStr->storage);
	
	SlangHeader* listObj = c->GetArg(1);
	if (!listObj){
		c->Return((SlangHeader*)c->alloc.AllocateStr(0));
		return true;
	}
	
	TYPE_CHECK_EXACT(listObj,SlangType::List);
	SlangList* joinList = (SlangList*)listObj;
	
	if (!joinList->right){
		SlangHeader* first = joinList->left;
		TYPE_CHECK_EXACT(first,SlangType::String);
		c->Return(first);
		return true;
	}
	
	size_t totalSize = 0;
	while (joinList){
		if (joinList->header.type!=SlangType::List){
			c->DotError();
			return false;
		}
		
		SlangHeader* jObj = joinList->left;
		TYPE_CHECK_EXACT(jObj,SlangType::String);
		SlangStr* jStr = (SlangStr*)jObj;
		totalSize += GetStorageSize(jStr->storage)+joinStrSize;
		
		joinList = (SlangList*)joinList->right;
	}
	totalSize -= joinStrSize;
	
	SlangStr* str = c->alloc.AllocateStr(totalSize);
	SlangStorage* storage = str->storage;
	joinStr = (SlangStr*)c->GetArg(0);
	joinList = (SlangList*)c->GetArg(1);
	SlangStorage* jStore = joinStr->storage;
	
	size_t pos = 0;
	SlangStr* first = (SlangStr*)joinList->left;
	if (first->storage){
		memcpy(storage->data,first->storage->data,first->storage->size);
		pos += first->storage->size;
	}
	joinList = (SlangList*)joinList->right;
	while (joinList){
		if (jStore){
			memcpy(storage->data+pos,jStore->data,joinStrSize);
			pos += joinStrSize;
		}
		
		SlangStr* jStr = (SlangStr*)joinList->left;
		if (jStr->storage){
			memcpy(storage->data+pos,jStr->storage->data,jStr->storage->size);
			pos += jStr->storage->size;
		}
		joinList = (SlangList*)joinList->right;
	}
	c->Return((SlangHeader*)str);
	return true;
}

bool CodeFuncMakeStrIStream(CodeInterpreter* c){
	SlangHeader* strObj = c->GetArg(0);
	TYPE_CHECK_EXACT(strObj,SlangType::String);
	
	SlangStr* str = (SlangStr*)strObj;
	
	SlangStream* stream = c->MakeStringInputStream(str);
	c->Return((SlangHeader*)stream);
	return true;
}

bool CodeFuncMakeStrOStream(CodeInterpreter* c){
	SlangStream* stream;
	if (!c->GetArgCount()){
		stream = c->MakeStringOutputStream(nullptr);
	} else {
		SlangHeader* strObj = c->GetArg(0);
		TYPE_CHECK_EXACT(strObj,SlangType::String);
		
		stream = c->MakeStringOutputStream((SlangStr*)strObj);
	}
	
	c->Return((SlangHeader*)stream);
	return true;
}

bool CodeFuncStreamGetStr(CodeInterpreter* c){
	SlangHeader* streamObj = c->GetArg(0);
	TYPE_CHECK_STREAM(streamObj);
	if (streamObj->isFile){
		c->StreamError("Stream has no underlying string!");
		return false;
	}
	
	SlangStream* stream = (SlangStream*)streamObj;
	c->Return((SlangHeader*)stream->str);
	return true;
}

bool CodeFuncStreamWrite(CodeInterpreter* c){
	SlangHeader* streamObj = c->GetArg(0);
	TYPE_CHECK_EXACT(streamObj,SlangType::OutputStream);
	
	SlangHeader* strObj = c->GetArg(1);
	TYPE_CHECK_EXACT(strObj,SlangType::String);
	
	if (GetStorageSize(((SlangStr*)strObj)->storage)==0){
		c->Return(streamObj);
		return true;
	}
	
	SlangStream* stream = (SlangStream*)streamObj;
	SlangStr* str = (SlangStr*)strObj;
	if (stream->header.isFile){
		if (!stream->file){
			c->FileError("Cannot write to a closed file!");
			return false;
		}
		fwrite(&str->storage->data[0],sizeof(uint8_t),str->storage->size,stream->file);
	} else {
		if (!c->SlangOutputToString(stream,strObj)){
			return false;
		}
	}
	
	c->Return(c->GetArg(0));
	return true;
}

bool CodeFuncStreamRead(CodeInterpreter* c){
	size_t argCount = c->GetArgCount();
	SlangHeader* streamObj = c->GetArg(0);
	TYPE_CHECK_EXACT(streamObj,SlangType::InputStream);
	bool isFile = streamObj->isFile;
	
	size_t count = UINT64_MAX;
	if (argCount==2){
		SlangHeader* intObj = c->GetArg(1);
		TYPE_CHECK_EXACT(intObj,SlangType::Int);
		
		ssize_t cval = ((SlangObj*)intObj)->integer;
		count = (cval < 0) ? 0 : cval;
	}
	
	SlangStream* stream = (SlangStream*)streamObj;
	//stream->pos = 0;
	if (isFile){
		if (!stream->file){
			c->FileError("Cannot read from a closed file!");
			return false;
		}
		
		std::string temp{};
		temp.reserve(64);
		int ch=0;
		while (count--&&(ch = fgetc(stream->file))!=EOF){
			temp.push_back(ch);
		}
		
		if (temp.empty()){
			c->Return(c->codeWriter.constEOFObj);
			return true;
		}
		
		SlangStr* intoStr = c->alloc.AllocateStr(temp.size());
		memcpy(&intoStr->storage->data[0],temp.data(),temp.size());
		c->Return((SlangHeader*)intoStr);
	} else {
		size_t bytesLeft = GetStorageSize(stream->str->storage)-stream->pos;
		if (bytesLeft==0){
			c->Return(c->codeWriter.constEOFObj);
			return true;
		}
		count = (count>bytesLeft) ? bytesLeft : count;
		
		SlangStr* intoStr = c->alloc.AllocateStr(count);
		stream = (SlangStream*)c->GetArg(0);
		
		assert(stream->str->storage);
		
		memcpy(intoStr->storage->data,&stream->str->storage->data[stream->pos],count);
		stream->pos += count;
		
		c->Return((SlangHeader*)intoStr);
	}
	return true;
}

bool CodeFuncStreamWriteByte(CodeInterpreter* c){
	SlangHeader* streamObj = c->GetArg(0);
	TYPE_CHECK_EXACT(streamObj,SlangType::OutputStream);
	
	SlangHeader* wObj = c->GetArg(1);
	TYPE_CHECK_EXACT(wObj,SlangType::Int);
	
	uint8_t byte = ((SlangObj*)wObj)->integer&0xFF;
	
	SlangStream* stream = (SlangStream*)streamObj;
	if (streamObj->isFile){
		if (!stream->file){
			c->FileError("Cannot write to a closed file!");
			return false;
		}
		fputc(byte,stream->file);
	} else {
		SlangStr* streamstr = stream->str;
		size_t currCap = GetStorageCapacity(streamstr->storage);
		
		// needs realloc
		if (currCap==stream->pos){
			size_t newCap = 1+GetStorageSize(streamstr->storage);
			newCap = newCap*3/2+8;
			SlangStorage* newStorage = c->alloc.AllocateStorage(newCap,sizeof(uint8_t));
			
			// fix pointers
			stream = (SlangStream*)c->GetArg(0);
			streamstr = stream->str;
			
			newStorage->size = streamstr->storage->size;
			memcpy(newStorage->data,streamstr->storage->data,sizeof(uint8_t)*streamstr->storage->size);
			streamstr->storage = newStorage;
		}
		
		if (stream->pos==streamstr->storage->size)
			streamstr->storage->size++;
		streamstr->storage->data[stream->pos++] = byte;
	}
	
	c->Return((SlangHeader*)stream);
	return true;
}

bool CodeFuncStreamReadByte(CodeInterpreter* c){
	SlangHeader* streamObj = c->GetArg(0);
	TYPE_CHECK_EXACT(streamObj,SlangType::InputStream);
	bool isFile = streamObj->isFile;
	
	SlangStream* stream = (SlangStream*)streamObj;
	if (isFile){
		if (!stream->file){
			c->FileError("Cannot read from a closed file!");
			return false;
		}
		int ch = fgetc(stream->file);
		if (ch==EOF){
			c->Return(c->codeWriter.constEOFObj);
			return true;
		}
		c->Return((SlangHeader*)c->alloc.MakeInt(ch&0xFF));
	} else {
		size_t bytesLeft = GetStorageSize(stream->str->storage)-stream->pos;
		if (bytesLeft==0){
			c->Return(c->codeWriter.constEOFObj);
			return true;
		}
		
		uint8_t byte = stream->str->storage->data[stream->pos++];
		c->Return((SlangHeader*)c->alloc.MakeInt(byte));
	}
	return true;
}

bool CodeFuncStreamSeekBegin(CodeInterpreter* c){
	SlangHeader* streamObj = c->GetArg(0);
	TYPE_CHECK_STREAM(streamObj);
	SlangStream* stream = (SlangStream*)streamObj;
	
	ssize_t offset = 0;
	if (c->GetArgCount()==2){
		SlangHeader* intObj = c->GetArg(1);
		TYPE_CHECK_EXACT(intObj,SlangType::Int);
		offset = ((SlangObj*)intObj)->integer;
	}
	
	if (stream->header.isFile){
		if (!stream->file){
			c->FileError("Cannot seek in a closed file!");
			return false;
		}
		fseek(stream->file,offset,SEEK_SET);
	} else {
		stream->pos = (offset<0) ? 0 : offset;
		if (stream->pos > GetStorageSize(stream->str->storage)){
			stream->pos = GetStorageSize(stream->str->storage);
		}
	}
	c->Return(streamObj);
	return true;
}

bool CodeFuncStreamSeekEnd(CodeInterpreter* c){
	SlangHeader* streamObj = c->GetArg(0);
	TYPE_CHECK_STREAM(streamObj);
	SlangStream* stream = (SlangStream*)streamObj;
	
	ssize_t offset = 0;
	if (c->GetArgCount()==2){
		SlangHeader* intObj = c->GetArg(1);
		TYPE_CHECK_EXACT(intObj,SlangType::Int);
		offset = ((SlangObj*)intObj)->integer;
	}
	
	if (stream->header.isFile){
		if (!stream->file){
			c->FileError("Cannot seek in a closed file!");
			return false;
		}
		fseek(stream->file,-offset,SEEK_END);
	} else {
		ssize_t ssize = GetStorageSize(stream->str->storage);
		if (offset>ssize){
			offset = ssize;
		} else if (offset<0){
			offset = 0;
		}
		stream->pos = ssize-offset;
	}
	c->Return(streamObj);
	return true;
}

bool CodeFuncStreamSeekOffset(CodeInterpreter* c){
	SlangHeader* streamObj = c->GetArg(0);
	TYPE_CHECK_STREAM(streamObj);
	SlangStream* stream = (SlangStream*)streamObj;
	
	ssize_t offset = 0;
	if (c->GetArgCount()==2){
		SlangHeader* intObj = c->GetArg(1);
		TYPE_CHECK_EXACT(intObj,SlangType::Int);
		offset = ((SlangObj*)intObj)->integer;
	}
	
	if (stream->header.isFile){
		if (!stream->file){
			c->FileError("Cannot seek in a closed file!");
			return false;
		}
		fseek(stream->file,offset,SEEK_CUR);
	} else {
		ssize_t ssize = GetStorageSize(stream->str->storage);
		ssize_t want = stream->pos+offset;
		
		if (want<0){
			want = 0;
		} else if (want>ssize){
			want = ssize;
		}
		stream->pos = want;
	}
	c->Return(streamObj);
	return true;
}

bool CodeFuncStreamTell(CodeInterpreter* c){
	SlangHeader* streamObj = c->GetArg(0);
	TYPE_CHECK_STREAM(streamObj);
	SlangStream* stream = (SlangStream*)streamObj;
	ssize_t t;
	if (stream->header.isFile){
		if (!stream->file){
			c->FileError("Cannot tell position of a closed file!");
			return false;
		}
		t = ftell(stream->file);
	} else {
		t = stream->pos;
	}
	c->Return((SlangHeader*)c->alloc.MakeInt(t));
	return true;
}

bool CodeFuncIsNull(CodeInterpreter* c){
	SlangHeader* arg = c->GetArg(0);
	if (arg)
		c->Return(c->codeWriter.constFalseObj);
	else
		c->Return(c->codeWriter.constTrueObj);
	return true;
}

bool CodeFuncIsInt(CodeInterpreter* c){
	SlangHeader* arg = c->GetArg(0);
	if (GetType(arg)==SlangType::Int)
		c->Return(c->codeWriter.constTrueObj);
	else
		c->Return(c->codeWriter.constFalseObj);
	return true;
}

bool CodeFuncIsReal(CodeInterpreter* c){
	SlangHeader* arg = c->GetArg(0);
	if (GetType(arg)==SlangType::Real)
		c->Return(c->codeWriter.constTrueObj);
	else
		c->Return(c->codeWriter.constFalseObj);
	return true;
}

bool CodeFuncIsNum(CodeInterpreter* c){
	SlangHeader* arg = c->GetArg(0);
	if (IsNumeric(arg))
		c->Return(c->codeWriter.constTrueObj);
	else
		c->Return(c->codeWriter.constFalseObj);
	return true;
}

bool CodeFuncIsStr(CodeInterpreter* c){
	SlangHeader* arg = c->GetArg(0);
	if (GetType(arg)==SlangType::String)
		c->Return(c->codeWriter.constTrueObj);
	else
		c->Return(c->codeWriter.constFalseObj);
	return true;
}

bool CodeFuncIsPair(CodeInterpreter* c){
	SlangHeader* arg = c->GetArg(0);
	if (GetType(arg)==SlangType::List)
		c->Return(c->codeWriter.constTrueObj);
	else
		c->Return(c->codeWriter.constFalseObj);
	return true;
}

bool CodeFuncIsProc(CodeInterpreter* c){
	SlangHeader* arg = c->GetArg(0);
	
	if (GetType(arg)==SlangType::Lambda)
		c->Return(c->codeWriter.constTrueObj);
	else if (GetType(arg)==SlangType::Symbol&&
			((SlangObj*)arg)->symbol<GLOBAL_SYMBOL_COUNT)
		c->Return(c->codeWriter.constTrueObj);
	else
		c->Return(c->codeWriter.constFalseObj);
	return true;
}

bool CodeFuncIsVec(CodeInterpreter* c){
	SlangHeader* arg = c->GetArg(0);
	if (GetType(arg)==SlangType::Vector)
		c->Return(c->codeWriter.constTrueObj);
	else
		c->Return(c->codeWriter.constFalseObj);
	return true;
}

bool CodeFuncIsMaybe(CodeInterpreter* c){
	SlangHeader* arg = c->GetArg(0);
	if (GetType(arg)==SlangType::Maybe)
		c->Return(c->codeWriter.constTrueObj);
	else
		c->Return(c->codeWriter.constFalseObj);
	return true;
}

bool CodeFuncIsEndOfFile(CodeInterpreter* c){
	SlangHeader* arg = c->GetArg(0);
	if (GetType(arg)==SlangType::EndOfFile)
		c->Return(c->codeWriter.constTrueObj);
	else
		c->Return(c->codeWriter.constFalseObj);
	return true;
}

bool CodeFuncIsBound(CodeInterpreter* c){
	SlangHeader* arg = c->GetArg(0);
	if (GetType(arg)!=SlangType::Symbol){
		c->TypeError(GetType(arg),SlangType::Symbol);
		return false;
	}
	SymbolName sym = ((SlangObj*)arg)->symbol;
	
	if (IsPredefinedSymbol(sym)){
		c->Return(c->codeWriter.constTrueObj);
		return true;
	}
	
	SlangHeader* d;
	if (GetRecursiveSymbol(c->GetCurrEnv(),sym,&d)){
		c->Return(c->codeWriter.constTrueObj);
	} else {
		c->Return(c->codeWriter.constFalseObj);
	}
	return true;
}

bool CodeFuncIsMain(CodeInterpreter* c){
	size_t func = c->funcStack.Back().funcIndex;
	if (c->codeWriter.lambdaCodes[func].moduleIndex==0){
		c->Return(c->codeWriter.constTrueObj);
	} else {
		c->Return(c->codeWriter.constFalseObj);
	}
	return true;
}

bool CodeFuncIntToReal(CodeInterpreter* c){
	SlangHeader* intObj = c->GetArg(0);
	TYPE_CHECK_EXACT(intObj,SlangType::Int);
	c->Return((SlangHeader*)c->alloc.MakeReal((double)((SlangObj*)intObj)->integer));
	return true;
}

bool CodeFuncRealToInt(CodeInterpreter* c){
	SlangHeader* realObj = c->GetArg(0);
	TYPE_CHECK_EXACT(realObj,SlangType::Real);
	c->Return((SlangHeader*)c->alloc.MakeInt((int64_t)((SlangObj*)realObj)->real));
	return true;
}

void CFGetImportName(CodeInterpreter* c,SlangList* nameList,std::string& name){
	assert(nameList);
	if (!name.empty())
		name.push_back('/');
	SymbolName sym = ((SlangObj*)nameList->left)->symbol;
	name += c->parser.GetSymbolString(sym);
	nameList = (SlangList*)nameList->right;
	
	while (nameList){
		name.push_back('/');
		sym = ((SlangObj*)nameList->left)->symbol;
		name += c->parser.GetSymbolString(sym);
		nameList = (SlangList*)nameList->right;
	}
}

bool CFHandleSpecialImport(CodeInterpreter* c,SlangList* nameList){
	SymbolName sym = ((SlangObj*)nameList->left)->symbol;
	if (sym!=c->parser.RegisterSymbol("slang")){
		return false;
	}
	nameList = (SlangList*)nameList->right;
	if (!nameList)
		return false;
	if (nameList->right)
		return false;
	sym = ((SlangObj*)nameList->left)->symbol;
	return c->ImportBuiltinModule(sym);
}

bool CFHandleImport(CodeInterpreter* c,SlangList* nameList){
	size_t funcIndex = c->funcStack.Back().funcIndex;
	uint32_t moduleName = c->codeWriter.lambdaCodes[funcIndex].moduleIndex;
	// make this faster
	std::string importName = GetBaseDir(c->GetModuleString(moduleName));
	CFGetImportName(c,nameList,importName);
	importName += SLANG_FILE_EXT;
	ModuleName mname = c->RegisterModuleName(importName);
	if (mname<c->modules.size){
		// just reimport
		SlangEnv* exportEnv = c->modules.data[mname].exportEnv;
		c->ImportEnv(exportEnv);
		return true;
	}
	
	std::ifstream f{importName,std::ios::ate};
	if (!f){
		if (CFHandleSpecialImport(c,nameList))
			return true;
			
		c->FileError("File does not exist!");
		return false;
	}
	auto size = f.tellg();
	f.seekg(0);
	std::string code = std::string(size,'\0');
	f.read(&code[0],size);
	ModuleData& md = c->modules.PlaceBack();
	md.exportEnv = nullptr;
	md.globalEnv = nullptr;
	size_t newFunc;
	if (!c->LoadModule(mname,code,newFunc))
		return false;
	
	md.exportEnv = c->AllocateEnvs(4);
	SlangEnv* newEnv = c->AllocateEnvs(4);
	md.globalEnv = newEnv;
	c->stack.PushBack({c->argStack.size});
	c->Call(newFunc,newEnv);
	return true;
}

bool CodeFuncExport(CodeInterpreter* c){
	SlangHeader* symObj = c->GetArg(0);
	TYPE_CHECK_EXACT(symObj,SlangType::Symbol);
	size_t funcIndex = c->funcStack.Back().funcIndex;
	uint32_t moduleName = c->codeWriter.lambdaCodes[funcIndex].moduleIndex;
	SlangEnv* exportEnv = c->modules.data[moduleName].exportEnv;
	if (!exportEnv)
		return true;
	
	SymbolName sym = ((SlangObj*)symObj)->symbol;
	
	SlangEnv* currEnv = c->GetCurrEnv();
	SlangHeader* val;
	if (!GetRecursiveSymbol(currEnv,sym,&val)){
		c->UndefinedError(sym);
		return false;
	}
	
	SlangHeader* d;
	if (exportEnv->GetSymbol(sym,&d)){
		c->ExportError(sym);
		return false;
	}
	c->DefEnvSymbol(exportEnv,sym,val);
	c->Return(nullptr);
	return true;
}

bool CodeFuncInvalid(CodeInterpreter* c){
	c->PushError("ApplyError","Invalid procedure!");
	return false;
}

const CodeFunc CodeBuiltinFuncs[] = {
	// tail recursive
	CodeFuncIf,
	CodeFuncInvalid, // cond
	CodeFuncInvalid, // case
	CodeFuncInvalid, // let
	CodeFuncInvalid, // letrec
	CodeFuncInvalid, // do
	CodeFuncApply,
	CodeFuncEval, // eval
	
	// non tail recursive
	CodeFuncInvalid, // def
	CodeFuncInvalid, // &
	CodeFuncInvalid, // set!
	CodeFuncLen,
	CodeFuncMap,
	CodeFuncAnd,
	CodeFuncOr,
	CodeFuncInvalid, // try
	CodeFuncParse,
	CodeFuncUnwrap,
	CodeFuncEmpty,
	CodeFuncQuote,
	CodeFuncInvalid, // quasiquote
	CodeFuncInvalid, // unquote
	CodeFuncInvalid, // unquote-splicing
	CodeFuncNot,
	CodeFuncList,
	CodeFuncListGet,
	CodeFuncListSet,
	CodeFuncListConcat,
	CodeFuncOutputTo,
	CodeFuncInputFrom,
	CodeFuncVec,
	CodeFuncVecAlloc,
	CodeFuncVecGet,
	CodeFuncVecSet,
	CodeFuncVecApp,
	CodeFuncVecPop,
	CodeFuncDict,
	CodeFuncDictGet,
	CodeFuncDictSet,
	CodeFuncDictPop,
	CodeFuncStrGet,
	CodeFuncStrSet,
	CodeFuncStrApp,
	CodeFuncStrPop,
	CodeFuncStrSplit,
	CodeFuncStrJoin,
	CodeFuncMakeStrIStream,
	CodeFuncMakeStrOStream,
	CodeFuncStreamGetStr,
	CodeFuncStreamWrite,
	CodeFuncStreamRead,
	CodeFuncStreamWriteByte,
	CodeFuncStreamReadByte,
	CodeFuncStreamSeekBegin,
	CodeFuncStreamSeekEnd,
	CodeFuncStreamSeekOffset,
	CodeFuncStreamTell,
	CodeFuncPrint,
	CodeFuncOutput,
	CodeFuncInput,
	CodeFuncPair,
	CodeFuncLeft,
	CodeFuncRight,
	CodeFuncSetLeft,
	CodeFuncSetRight,
	CodeFuncInc,
	CodeFuncDec,
	CodeFuncAdd,
	CodeFuncSub,
	CodeFuncMul,
	CodeFuncDiv,
	CodeFuncMod,
	CodeFuncPow,
	CodeFuncAbs,
	CodeFuncFloor,
	CodeFuncCeil,
	CodeFuncMin,
	CodeFuncMax,
	CodeFuncBitAnd,
	CodeFuncBitOr,
	CodeFuncBitXor,
	CodeFuncBitNot,
	CodeFuncBitLeftShift,
	CodeFuncBitRightShift,
	CodeFuncGT, //SlangFuncGT,
	CodeFuncLT, //SlangFuncLT,
	CodeFuncGTE, //SlangFuncGTE,
	CodeFuncLTE, //SlangFuncLTE,
	CodeFuncAssert,
	CodeFuncEq,
	CodeFuncNEq,
	CodeFuncIs,
	CodeFuncIsNull,
	CodeFuncIsInt,
	CodeFuncIsReal,
	CodeFuncIsNum,
	CodeFuncIsStr,
	CodeFuncIsPair,
	CodeFuncIsProc,
	CodeFuncIsVec,
	CodeFuncIsMaybe,
	CodeFuncIsEndOfFile,
	CodeFuncIsBound,
	CodeFuncInvalid, // pure?
	CodeFuncIsMain,
	CodeFuncIntToReal,
	CodeFuncRealToInt,
	CodeFuncExport,
	CodeFuncInvalid,
};
static_assert(SL_ARR_LEN(CodeBuiltinFuncs)==GLOBAL_SYMBOL_COUNT);

inline bool CFHandleArgs(CodeInterpreter* c,SlangEnv* lamEnv,const CodeBlock& cb){
	if (cb.isVariadic){
		c->lamEnv = lamEnv;
		size_t base = c->stack.Back().base;
		size_t argCount = c->argStack.size-base;
		assert(cb.params.size>0);
		if ((size_t)(cb.params.size-1)>argCount){
			c->ArityError(argCount,cb.params.size,VARIADIC_ARG_COUNT);
			return false;
		}
	
		size_t vaStart = base+cb.params.size-1;
		size_t vaEnd = c->argStack.size;
		SlangList* vaList = c->MakeVariadicList(vaStart,vaEnd);
		c->argStack.size = vaStart;
		c->PushArg((SlangHeader*)vaList);
		
		if (cb.isClosure){
			argCount = c->argStack.size-base;
			lamEnv = c->lamEnv;
			
			SlangEnv* envIt = lamEnv;
			for (size_t i=0;i<argCount;++i){
				auto& mapping = envIt->mappings[i%SLANG_ENV_BLOCK_SIZE];
				mapping.obj = c->argStack.data[base+i];
				
				if (i%SLANG_ENV_BLOCK_SIZE==SLANG_ENV_BLOCK_SIZE-1)
					envIt = envIt->next;
			}
		}
		return true;
	} else {
		size_t base = c->stack.Back().base;
		size_t argCount = c->argStack.size-base;
		if (cb.params.size!=argCount){
			c->ArityError(argCount,cb.params.size,cb.params.size);
			return false;
		}
		if (cb.isClosure){
			SlangEnv* envIt = lamEnv;
			for (size_t i=0;i<argCount;++i){
				auto& mapping = envIt->mappings[i%SLANG_ENV_BLOCK_SIZE];
				mapping.obj = c->argStack.data[base+i];
				
				if (i%SLANG_ENV_BLOCK_SIZE==SLANG_ENV_BLOCK_SIZE-1)
					envIt = envIt->next;
			}
		}
	}
	return true;
}

inline bool MapInnerLoop(CodeInterpreter* c,size_t listsBase,size_t argCount){
	for (size_t i=0;i<argCount;++i){
		if (GetType(c->argStack.data[listsBase+i])!=SlangType::List){
			return false;
		}
		c->PushArg(((SlangList*)c->argStack.data[listsBase+i])->left);
		c->argStack.data[listsBase+i] = ((SlangList*)c->argStack.data[listsBase+i])->right;
	}
	
	return true;
}

bool CodeFuncMap(CodeInterpreter* c){
	SlangHeader* funcArg = c->GetArg(0);
	size_t listsBase = c->stack.Back().base+1;
	size_t argCount = c->GetArgCount()-1;
	if (GetType(funcArg)!=SlangType::Symbol){
		c->TypeError(GetType(funcArg),SlangType::Lambda);
		return false;
	}
	
	for (size_t i=0;i<argCount;++i){
		SlangHeader* arg = c->GetArg(i+1);
		if (!IsList(arg)){
			c->TypeError(arg->type,SlangType::List);
			return false;
		}
	}
	
	size_t resIndex = c->argStack.size;
	size_t currIndex = c->argStack.size+1;
	SlangList* resL = c->alloc.AllocateList();
	resL->left = nullptr;
	resL->right = nullptr;
	c->PushArg((SlangHeader*)resL);
	c->PushArg((SlangHeader*)resL);
	funcArg = c->GetArg(0);
	
	c->PushFrame();
	size_t argFrameStart = c->argStack.size;
	if (!MapInnerLoop(c,listsBase,argCount)){
		c->Return(nullptr);
		return true;
	}
	if (funcArg->type==SlangType::Symbol&&
		((SlangObj*)funcArg)->symbol<GLOBAL_SYMBOL_COUNT){
		SymbolName sym = ((SlangObj*)funcArg)->symbol;
		if (!GoodArity(sym,argCount)){
			c->ArityError(argCount,
				gGlobalArityArray[sym].min,
				gGlobalArityArray[sym].max
			);
			return false;
		}
		
		const CodeFunc f = CodeBuiltinFuncs[sym];
		while (true){
			if (!f(c))
				return false;
			
			((SlangList*)c->argStack.data[currIndex])->left = c->PopArg();
			
			c->argStack.size = argFrameStart;
			c->PushFrame();
			if (!MapInnerLoop(c,listsBase,argCount))
				break;
			SlangList* newList = c->alloc.AllocateList();
			newList->left = nullptr;
			newList->right = nullptr;
			((SlangList*)c->argStack.data[currIndex])->right = (SlangHeader*)newList;
			c->argStack.data[currIndex] = (SlangHeader*)newList;
		}
	} else {
		c->TypeError(funcArg->type,SlangType::Lambda);
		return false;
	}
	c->PopFrame();
	
	c->Return(c->argStack.data[resIndex]);
	return true;
}

bool CodeFuncApply(CodeInterpreter* c){
	SlangHeader* func = c->GetArg(0);
	SlangType fType = GetType(func);
	if (!((fType==SlangType::Symbol&&((SlangObj*)func)->symbol<GLOBAL_SYMBOL_COUNT)||
		fType==SlangType::Lambda)){
		c->TypeError(fType,SlangType::Lambda);
		return false;
	}
	
	size_t argCount = c->GetArgCount()-2;
	size_t appArgCount = 0;
	for (size_t i=0;i<argCount;++i){
		c->SetArg(i,c->GetArg(1+i));
		++appArgCount;
	}
	SlangHeader* vArg = c->GetArg(argCount+1);
	if (!IsList(vArg)){
		c->TypeError(vArg->type,SlangType::List);
		return false;
	}
	
	while (vArg){
		c->SetArg(appArgCount,((SlangList*)vArg)->left);
		vArg = ((SlangList*)vArg)->right;
		if (!IsList(vArg)){
			c->DotError();
			return false;
		}
		++appArgCount;
	}
	
	c->argStack.size = c->stack.Back().base+appArgCount;
	
	if (fType==SlangType::Symbol){
		SymbolName sym = ((SlangObj*)func)->symbol;
		[[unlikely]]
		if (!GoodArity(sym,appArgCount)){
			c->ArityError(appArgCount,gGlobalArityArray[sym].min,gGlobalArityArray[sym].max);
			return false;
		}
		[[unlikely]]
		if (!CodeBuiltinFuncs[sym](c))
			return false;
	} else {
		SlangLambda* lam = (SlangLambda*)func;
		size_t funcIndex = lam->funcIndex;
		c->lamEnv = lam->env;
		[[unlikely]]
		if (!CFHandleArgs(c,
							lam->env,
							c->codeWriter.lambdaCodes[funcIndex]))
			return false;
			
		c->Call(funcIndex,c->lamEnv);
		c->lamEnv = nullptr;
	}
	
	return true;
}

#define LOOP_TYPE_CHECK_NUMERIC(expr) \
	if (!IsNumeric(expr)){ \
		c->TypeError2(GetType(expr),SlangType::Int,SlangType::Real); \
		return; \
	}
	
#define LOOP_TYPE_CHECK_EXACT(expr,type) \
	if (GetType(expr)!=(type)){ \
		c->TypeError(GetType(expr),(type)); \
		return; \
	}

#define NEXT_INST() \
	c->pc += SlangOpSizes[op]; \
	op = *c->pc; \
	++stepCount; \
	goto *blocks[op];

#define LOOP_NAME GotoLoop

#define LOOP_PREAMBLE() \
	static const void* blocks[] = { \
		&&SLANG_OP_NOOP_label, \
		&&SLANG_OP_HALT_label, \
		&&SLANG_OP_NULL_label, \
		&&SLANG_OP_LOAD_PTR_label, \
		&&SLANG_OP_BOOL_TRUE_label, \
		&&SLANG_OP_BOOL_FALSE_label, \
		&&SLANG_OP_ZERO_label, \
		&&SLANG_OP_ONE_label, \
		&&SLANG_OP_PUSH_LAMBDA_label, \
		\
		&&SLANG_OP_LOOKUP_label, \
		&&SLANG_OP_SET_label, \
		&&SLANG_OP_GET_LOCAL_label, \
		&&SLANG_OP_SET_LOCAL_label, \
		&&SLANG_OP_GET_GLOBAL_label, \
		&&SLANG_OP_SET_GLOBAL_label, \
		&&SLANG_OP_DEF_GLOBAL_label, \
		&&SLANG_OP_GET_REC_label, \
		&&SLANG_OP_SET_REC_label, \
		\
		&&SLANG_OP_PUSH_FRAME_label, \
		&&SLANG_OP_POP_ARG_label, \
		&&SLANG_OP_UNPACK_label, \
		&&SLANG_OP_COPY_label, \
		&&SLANG_OP_CALL_label, \
		&&SLANG_OP_CALLSYM_label, \
		&&SLANG_OP_RET_label, \
		&&SLANG_OP_RETCALL_label, \
		&&SLANG_OP_RETCALLSYM_label, \
		&&SLANG_OP_RECURSE_label, \
		\
		&&SLANG_OP_JUMP_label, \
		&&SLANG_OP_CJUMP_POP_label, \
		&&SLANG_OP_CNJUMP_POP_label, \
		&&SLANG_OP_CJUMP_label, \
		&&SLANG_OP_CNJUMP_label, \
		&&SLANG_OP_CASE_JUMP_label, \
		&&SLANG_OP_TRY_label, \
		&&SLANG_OP_MAYBE_NULL_label, \
		&&SLANG_OP_MAYBE_WRAP_label, \
		&&SLANG_OP_MAYBE_UNWRAP_label, \
		\
		&&SLANG_OP_MAP_STEP_label, \
		\
		&&SLANG_OP_NOT_label, \
		&&SLANG_OP_INC_label, \
		&&SLANG_OP_DEC_label, \
		&&SLANG_OP_NEG_label, \
		&&SLANG_OP_INVERT_label, \
		&&SLANG_OP_ADD_label, \
		&&SLANG_OP_SUB_label, \
		&&SLANG_OP_MUL_label, \
		&&SLANG_OP_DIV_label, \
		&&SLANG_OP_EQ_label, \
		&&SLANG_OP_PAIR_label, \
		&&SLANG_OP_LIST_CONCAT_label, \
		&&SLANG_OP_LEFT_label, \
		&&SLANG_OP_RIGHT_label, \
		&&SLANG_OP_SET_LEFT_label, \
		&&SLANG_OP_SET_RIGHT_label, \
		&&SLANG_OP_MAKE_VEC_label, \
		&&SLANG_OP_VEC_GET_label, \
		&&SLANG_OP_VEC_SET_label, \
		&&SLANG_OP_EXPORT_label, \
		&&SLANG_OP_IMPORT_label, \
	}; \
	static_assert(SL_ARR_LEN(blocks)==SLANG_OP_COUNT); \
	op = *c->pc; \
	goto *blocks[op]; \
	while (true){
	
#define LOOP_POST() }
#define LOOP_INST(inst) inst ## _label:
#include "loop.cpp.inc"
#undef LOOP_NAME
#undef LOOP_PREAMBLE
#undef LOOP_POST
#undef LOOP_INST
#undef NEXT_INST

#define LOOP_NAME SwitchLoop
#define NEXT_INST() break
#define LOOP_INST(inst) case inst:
#define LOOP_PREAMBLE() \
	op = *c->pc; \
	while (true){ \
	switch (op){
#define LOOP_POST() \
		} \
		c->pc += SlangOpSizes[op]; \
		op = *c->pc; \
		++stepCount; \
	}
#include "loop.cpp.inc"

#undef LOOP_NAME
#undef LOOP_PREAMBLE
#undef LOOP_POST
#undef LOOP_INST
#undef NEXT_INST


#define LOOP_NAME SingleStep
#define NEXT_INST() break
#define LOOP_INST(inst) case inst:
#define LOOP_PREAMBLE() \
	op = *c->pc; \
	switch (op){
#define LOOP_POST() \
		} \
		c->pc += SlangOpSizes[op];
#include "loop.cpp.inc"

#undef LOOP_NAME
#undef LOOP_PREAMBLE
#undef LOOP_POST
#undef LOOP_INST
#undef NEXT_INST

inline bool CodeInterpreter::InlineStep(){
	SingleStep(this);
	++stepCount;
	if (!errors.empty()){
		if (tryStack.size==0)
			halted = true;
		else 
			LoadTry();
	}
	return !halted;
}

bool CodeInterpreter::Step(){
	return InlineStep();
}

bool CodeInterpreter::CompileInto(SlangHeader* prog,size_t funcIndex){
	codeWriter.errors.clear();
	codeWriter.curr = &codeWriter.lambdaCodes[funcIndex];
	codeWriter.curr->write = codeWriter.curr->start;
	codeWriter.curr->locData.size = 0;
	codeWriter.curr->moduleIndex = 0;
	if (!codeWriter.CompileExpr(prog)){
		return false;
	}
	codeWriter.WriteOpCode(SLANG_OP_HALT);
	return true;
}

void CodeInterpreter::ResetState(){
	errors.clear();
	lamEnv = nullptr;
	halted = false;
	pc = nullptr;
	stepCount = 0;
	gMaxStackHeight = 0;
	gSmallGCs = 0;
	
	stack.Clear();
	funcStack.Clear();
	argStack.Clear();
	tryStack.Clear();
}

void CodeInterpreter::SetupDefaultModule(const std::string& name){
	currModuleName = 0;
	modules.Clear();
	moduleNameDict.clear();
	SlangEnv* global = AllocateEnvs(4);
	ModuleData& md = modules.PlaceBack();
	md.globalEnv = global;
	md.exportEnv = nullptr;
	RegisterModuleName(name);
}

bool CodeInterpreter::LoadExpr(const std::string& code){
	ResetState();
	
	if (modules.size==0){
		SetupDefaultModule("<repl>");
	}
	
	parser.SetCodeString(code);
	SlangHeader* prog;
	if (!parser.ParseLine(&prog))
		return false;
		
	if (parser.token.type!=SlangTokenType::EndOfFile)
		return false;
		
	codeWriter.currModuleIndex = 0;
	if (!CompileInto(prog,0)){
		return false;
	}
	
	SlangEnv* global = modules.Front().globalEnv;
	// set up vars
	StackData& s = stack.PlaceBack();
	s.base = 0;
	FuncData& f = funcStack.PlaceBack();
	f.globalEnv = global;
	f.env = global;
	f.argsFrame = 0;
	f.funcIndex = 0;
	f.retAddr = 0;
	pc = codeWriter.lambdaCodes[f.funcIndex].start;
	
	return true;
}

bool CodeInterpreter::LoadProgram(const std::string& filename,const std::string& code){
	gRandState.Seed(GetSeedTime());
	ResetState();
	codeWriter.Reset();
	
	SetupDefaultModule(filename);
	
	if (!codeWriter.CompileCode(code))
		return false;
	
	SlangEnv* global = modules.Front().globalEnv;
	// set up vars
	StackData& s = stack.PlaceBack();
	s.base = 0;
	FuncData& fd = funcStack.PlaceBack();
	fd.globalEnv = global;
	fd.env = global;
	fd.argsFrame = 0;
	fd.funcIndex = 0;
	fd.retAddr = 0;
	pc = codeWriter.lambdaCodes[fd.funcIndex].start;
	
	return true;
}

bool CodeInterpreter::LoadModule(ModuleName name,const std::string& code,size_t& funcIndex){
	if (!codeWriter.CompileModule(name,code,funcIndex)){
		PushError("CompileError","Could not compile imported module!");
		return false;
	}
	
	return true;
}

bool CodeInterpreter::Run(){
	struct timespec tstart,tend;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&tstart);
	
	halted = false;
	//while (InlineStep()){}
	bool success = true;
	while (true){
		GotoLoop(this);
		if (halted)
			break;
		if (tryStack.size==0){
			success = false;
			break;
		}
		LoadTry();
	}
	
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&tend);
	gRunTime = (tend.tv_sec+1e-9*tend.tv_nsec)-
				(tstart.tv_sec+1e-9*tstart.tv_nsec);
	
	if (!success){
		while (funcStack.size>1){
			auto& frame = funcStack.Back();
			pc = frame.retAddr;
			funcStack.PopBack();
			EvalError();
		}
	}
	return success;
}


inline void* CodeInterpreterConstAllocate(void* data,size_t mem){
	MemChain* chain = (MemChain*)data;
	void* d = chain->Allocate(mem);
	return d;
}

CodeInterpreter::CodeInterpreter() 
	: codeWriter(parser),
	  alloc(this,CodeInterpreterAllocate),
	  memChain(8192),
	  constAlloc(&memChain,CodeInterpreterConstAllocate),
	  evalMemChain(4096),
	  evalAlloc(&evalMemChain,CodeInterpreterConstAllocate){
	gRandState.Seed(GetSeedTime());
	arena = new MemArena();
	
	size_t memSize = SMALL_SET_SIZE*2;
	uint8_t* memAlloc = (uint8_t*)malloc(memSize);
	arena->SetSpace(memAlloc,memSize,memAlloc);
	gArenaSize = memSize;
	gMaxArenaSize = memSize;
	
	stack.Reserve(256);
	funcStack.Reserve(256);
	argStack.Reserve(2048);
	tryStack.Reserve(16);
	modules.Reserve(16);
	finalizers.Reserve(8);
	gDebugInterpreter = this;
	
	InitBuiltinModules();
	
	ResetState();
}

CodeInterpreter::~CodeInterpreter(){
	for (size_t i=0;i<finalizers.size;++i){
		auto& finalizer = finalizers.data[i];
		finalizer.func(this,finalizer.obj);
	}
	
	if (arena->memSet)
		free(arena->memSet);
	delete arena;
	gDebugInterpreter = nullptr;
}

bool PrintListException(std::ostream& os,const SlangList* list){
	if (GetType(list->left)!=SlangType::Symbol) return false;
	size_t argCount = GetArgCount(list)-1;
	if (argCount!=1) return false;
	SlangObj* l = (SlangObj*)list->left;
	SymbolName sym = l->symbol;
	SlangHeader* arg = ((SlangList*)list->right)->left;
	switch (sym){
		case SLANG_QUOTE:
			os << "'";
			os << *arg;
			return true;
		case SLANG_QUASIQUOTE:
			os << "`";
			os << *arg;
			return true;
		case SLANG_UNQUOTE:
			os << ",";
			os << *arg;
			return true;
		case SLANG_UNQUOTE_SPLICING:
			os << "@";
			os << *arg;
			return true;
		default:
			break;
	}
	return false;
}

std::ostream& operator<<(std::ostream& os,const SlangHeader& obj){
	double r;
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
		case SlangType::DictTable:
			os << "[TABLE]";
			break;
		case SlangType::NullType:
			os << "[NULL]";
			break;
		case SlangType::EndOfFile:
			os << "#eof";
			break;
		case SlangType::InputStream:
			os << "[istream]";
			break;
		case SlangType::OutputStream:
			os << "[ostream]";
			break;
		case SlangType::Int:
			os << ((SlangObj*)&obj)->integer;
			break;
		case SlangType::Real:
			r = ((SlangObj*)&obj)->real;
			os << r;
			if (r==floor(r)&&abs(r)<1e16)
				os << ".0";
			break;
		case SlangType::Maybe:
			if (obj.flags & FLAG_MAYBE_OCCUPIED){
				SlangHeader* maybe = ((SlangObj*)&obj)->maybe;
				if (!maybe){
					os << "?<()>";
				} else {
					os << "?<" << *maybe << ">";
				}
			} else {
				os << "?<>";
			}
			break;
		case SlangType::Lambda: {
			bool variadic = obj.flags&FLAG_VARIADIC;
			bool closure = obj.flags&FLAG_CLOSURE;
			bool ext = obj.flags&FLAG_EXTERNAL;
			SlangLambda* lam = (SlangLambda*)&obj;
			os << "[& ";
			if (ext){
				const ExternalFuncData* efd = lam->extFunc;
				os << efd->name;
			} else {
				if (variadic)
					os << "VARG, ";
				if (closure)
					os << "CLOSURE, ";
				os << "IDX " << lam->funcIndex;
			}
			os << "]";
			break;
		}
		case SlangType::Symbol:
			os << gDebugParser->GetSymbolString(((SlangObj*)&obj)->symbol);
			break;
		case SlangType::String: {
			SlangStorage* storage = ((SlangStr*)&obj)->storage;
			os << '"';
			uint8_t c;
			if (storage){
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
			}
			os << '"';
			break;
		}
		case SlangType::Vector: {
			SlangStorage* storage = ((SlangVec*)&obj)->storage;
			
			os << "#[";
			if (storage){
				for (size_t i=0;i<storage->size;++i){
					if (i!=0)
						os << ' ';
					if (!storage->objs[i])
						os << "()";
					else
						os << *storage->objs[i];
				}
			}
			os << "]";
			break;
		}
		case SlangType::Dict: {
			SlangStorage* storage = ((SlangDict*)&obj)->storage;
			
			os << "#{";
			if (storage){
				bool spaced = false;
				for (size_t i=0;i<storage->size;++i){
					SlangDictElement* elem = &storage->elements[i];
					if ((uint64_t)elem->key == DICT_UNOCCUPIED_VAL)
						continue;
					if (!spaced){
						spaced = true;
					} else {
						os << ' ';
					}
					
					os << "(";
					if (elem->key)
						os << *elem->key;
					else
						os << "()";
					
					os << " . ";
					
					if (elem->val)
						os << *elem->val;
					else
						os << "()";
					os << ")";
				}
			}
			os << "}";
			break;
		}
		case SlangType::Bool:
			if (obj.boolVal){
				os << "true";
			} else {
				os << "false";
			}
			break;
		case SlangType::List: {
			SlangList* list = (SlangList*)&obj;
			if (PrintListException(os,list))
				break;
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
