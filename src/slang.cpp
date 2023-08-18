#include "slang.h"

#include <assert.h>
#include <iostream>
#include <cstring>
#include <sstream>
#include <math.h>
#include <time.h>
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
size_t gArenaSize = 0;
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

inline bool IsIdentifierChar(char c){
	return (c>='A'&&c<='Z') ||
		   (c>='a'&&c<='z') ||
		   (c>='0'&&c<='9') ||
		   c=='_'||c=='-'||c=='?'||
		   c=='!'||c=='@'||c=='&'||
		   c=='*'||c=='+'||c=='/'||
		   c=='$'||c=='%'||c=='^'||
		   c=='~'||c=='='||c=='.'||
		   c=='>'||c=='<'||c=='|';
}

enum SlangGlobalSymbol {
	SLANG_DEFINE = 0,
	SLANG_LAMBDA,
	SLANG_LET,
	SLANG_SET,
	SLANG_IF,
	SLANG_COND,
	SLANG_CASE,
	SLANG_ELSE,
	SLANG_DO,
	SLANG_AND,
	SLANG_OR,
	SLANG_LEN,
	SLANG_APPLY,
	SLANG_PARSE,
	SLANG_EVAL,
	SLANG_TRY,
	SLANG_UNWRAP,
	SLANG_EMPTY,
	SLANG_QUOTE,
	SLANG_NOT,
	SLANG_NEG,
	SLANG_LIST,
	SLANG_OUTPUT_TO,
	SLANG_INPUT_FROM,
	SLANG_VEC,
	SLANG_VEC_ALLOC,
	SLANG_VEC_GET,
	SLANG_VEC_SET,
	SLANG_VEC_APPEND,
	SLANG_VEC_POP,
	SLANG_STR_GET,
	SLANG_STR_SET,
	SLANG_MAKE_STR_ISTREAM,
	SLANG_MAKE_STR_OSTREAM,
	SLANG_STREAM_GET_STR,
	SLANG_STREAM_WRITE,
	SLANG_STREAM_READ,
	SLANG_STREAM_WRITE_BYTE,
	SLANG_STREAM_READ_BYTE,
	SLANG_STREAM_SEEK_BEGIN,
	SLANG_STREAM_SEEK_END,
	SLANG_STREAM_SEEK_OFFSET,
	SLANG_STREAM_TELL,
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
	SLANG_FLOOR_DIV,
	SLANG_MOD,
	SLANG_POW,
	SLANG_ABS,
	SLANG_FLOOR,
	SLANG_CEIL,
	SLANG_BITAND,
	SLANG_BITOR,
	SLANG_BITXOR,
	SLANG_BITNOT,
	SLANG_LEFTSHIFT,
	SLANG_RIGHTSHIFT,
	SLANG_GT,
	SLANG_LT,
	SLANG_GTE,
	SLANG_LTE,
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
	SLANG_IS_MAYBE,
	SLANG_IS_BOUND,
	SLANG_IS_EOF,
	GLOBAL_SYMBOL_COUNT
};

SymbolNameDict gDefaultNameDict = {
	{"def",SLANG_DEFINE},
	{"&",SLANG_LAMBDA},
	{"let",SLANG_LET},
	{"set!",SLANG_SET},
	{"if",SLANG_IF},
	{"cond",SLANG_COND},
	{"case",SLANG_CASE},
	{"else",SLANG_ELSE},
	{"do",SLANG_DO},
	{"and",SLANG_AND},
	{"or",SLANG_OR},
	{"len",SLANG_LEN},
	{"apply",SLANG_APPLY},
	{"parse",SLANG_PARSE},
	{"eval",SLANG_EVAL},
	{"quote",SLANG_QUOTE},
	{"not",SLANG_NOT},
	{"neg",SLANG_NEG},
	{"try",SLANG_TRY},
	{"unwrap",SLANG_UNWRAP},
	{"empty?",SLANG_EMPTY},
	{"output-to!",SLANG_OUTPUT_TO},
	{"input-from!",SLANG_INPUT_FROM},
	{"list",SLANG_LIST},
	{"vec",SLANG_VEC},
	{"vec-alloc",SLANG_VEC_ALLOC},
	{"vec-get",SLANG_VEC_GET},
	{"vec-set!",SLANG_VEC_SET},
	{"vec-app!",SLANG_VEC_APPEND},
	{"vec-pop!",SLANG_VEC_POP},
	{"str-get",SLANG_STR_GET},
	{"str-set!",SLANG_STR_SET},
	{"make-str-istream",SLANG_MAKE_STR_ISTREAM},
	{"make-str-ostream",SLANG_MAKE_STR_OSTREAM},
	{"stream-get-str",SLANG_STREAM_GET_STR},
	{"write!",SLANG_STREAM_WRITE},
	{"read!",SLANG_STREAM_READ},
	{"write-byte!",SLANG_STREAM_WRITE_BYTE},
	{"read-byte!",SLANG_STREAM_READ_BYTE},
	{"seek!",SLANG_STREAM_SEEK_BEGIN},
	{"seek-end!",SLANG_STREAM_SEEK_END},
	{"seek-off!",SLANG_STREAM_SEEK_OFFSET},
	{"tell",SLANG_STREAM_TELL},
	{"pair",SLANG_PAIR},
	{"L",SLANG_LEFT},
	{"R",SLANG_RIGHT},
	{"set-L!",SLANG_SET_LEFT},
	{"set-R!",SLANG_SET_RIGHT},
	{"++",SLANG_INC},
	{"--",SLANG_DEC},
	{"+",SLANG_ADD},
	{"-",SLANG_SUB},
	{"*",SLANG_MUL},
	{"/",SLANG_DIV},
	{"div",SLANG_FLOOR_DIV},
	{"%",SLANG_MOD},
	{"^",SLANG_POW},
	{"abs",SLANG_ABS},
	{"floor",SLANG_FLOOR},
	{"ceil",SLANG_CEIL},
	{"bitand",SLANG_BITAND},
	{"bitor",SLANG_BITOR},
	{"bitxor",SLANG_BITXOR},
	{"bitnot",SLANG_BITNOT},
	{"<<",SLANG_LEFTSHIFT},
	{">>",SLANG_RIGHTSHIFT},
	{">",SLANG_GT},
	{"<",SLANG_LT},
	{">=",SLANG_GTE},
	{"<=",SLANG_LTE},
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
	{"maybe?",SLANG_IS_MAYBE},
	{"bound?",SLANG_IS_BOUND},
	{"eof?",SLANG_IS_EOF}
};


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
	return ts.tv_sec + ((double)ts.tv_nsec)*1e-9;
}

inline double GetDoublePerfTime(){
	using tp = std::chrono::steady_clock::time_point;
	tp now = std::chrono::steady_clock::now();
	uint64_t t = 
		std::chrono::duration_cast<std::chrono::microseconds>(
			now.time_since_epoch()
		).count();
	
	return (double)t/1000000;
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
		case SlangType::InputStream:
			return "IStream";
		case SlangType::OutputStream:
			return "OStream";
		case SlangType::EndOfFile:
			return "EndOfFile";
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
			SlangStorage* storage = (SlangStorage*)this;
			return sizeof(SlangStorage)+storage->capacity*elemSize;
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
		std::cout << "tried to allocate " << mem << " bytes\n";
		std::cout << "already have " << arena->currPointer-arena->currSet << " in curr set\n";
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
	StackAddArg((SlangHeader*)storage);
	
	if (elemSize==sizeof(SlangHeader*)){
		SlangType fillType = GetType(fill);
		
		if (fillType==SlangType::List||fillType==SlangType::Vector||
			fillType==SlangType::Lambda||fillType==SlangType::String){
			
			SlangHeader** start = storage->objs;
			SlangHeader** end = start+size;
			while (start!=end){
				*start = fill;
				++start;
			}
		} else {
			size_t i=0;
			while (i<size){
				SlangHeader* copied = Copy(fill);
				storage = (SlangStorage*)funcArgStack[frameIndex-1];
				storage->objs[i++] = copied;
			}
		}
	}
	return (SlangStorage*)funcArgStack[--frameIndex];
}

inline SlangVec* SlangInterpreter::AllocateVec(size_t size,SlangHeader* fill){
	SlangVec* obj = (SlangVec*)Allocate(sizeof(SlangVec));
	obj->header.type = SlangType::Vector;
	obj->header.flags = 0;
	obj->storage = nullptr;
	if (size==0){
		return obj;
	}
	
	StackAddArg((SlangHeader*)obj);
	SlangStorage* s = AllocateStorage(size,sizeof(SlangHeader*),fill);
	obj = (SlangVec*)funcArgStack[--frameIndex];
	obj->storage = s;
	return obj;
}

inline SlangStr* SlangInterpreter::AllocateStr(size_t size){
	SlangStr* obj = (SlangStr*)Allocate(sizeof(SlangStr));
	obj->header.type = SlangType::String;
	obj->header.flags = 0;
	obj->storage = nullptr;
	if (size==0){
		return obj;
	}
	StackAddArg((SlangHeader*)obj);
	SlangStorage* s = AllocateStorage(size,sizeof(uint8_t),nullptr);
	obj = (SlangStr*)funcArgStack[--frameIndex];
	obj->storage = s;
	return obj;
}

inline SlangStr* SlangInterpreter::ReallocateStr(SlangStr* str,size_t newSize){
	newSize = QuantizeSize(newSize);
	SlangStorage* storage = str->storage;
	if (!storage){
		if (newSize==0) return str;
		
		StackAddArg((SlangHeader*)str);
		SlangStorage* newStorage = AllocateStorage(newSize,sizeof(uint8_t),nullptr);
		newStorage->size = 0;
		str = (SlangStr*)funcArgStack[--frameIndex];
		str->storage = newStorage;
		return str;
	}
	
	// shrink
	if (newSize<=storage->capacity){
		storage->capacity = newSize;
		storage->size = (storage->size > storage->capacity) ? storage->capacity : storage->size;
		return str;
	}
	
	StackAddArg((SlangHeader*)str);
	SlangStorage* newStorage = AllocateStorage(newSize,sizeof(uint8_t),nullptr);
	str = (SlangStr*)funcArgStack[--frameIndex];
	memcpy(&newStorage->data[0],&str->storage->data[0],str->storage->size*sizeof(uint8_t));
	newStorage->size = str->storage->size;
	str->storage = newStorage;
	return str;
}

inline SlangStream* SlangInterpreter::ReallocateStream(SlangStream* stream,size_t addLen){
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
	
	StackAddArg((SlangHeader*)stream);
	size_t newSize = (currCap+sizeIncr)*3/2+8;
	SlangStr* newStr = ReallocateStr(stream->str,newSize);
	newStr->storage->size = currSize+sizeIncr;
	stream = (SlangStream*)funcArgStack[--frameIndex];
	assert(stream->str==newStr);
	stream->str = newStr;
	return stream;
}

inline SlangStream* SlangInterpreter::AllocateStream(SlangType type){
	SlangStream* stream = (SlangStream*)Allocate(sizeof(SlangStream));
	stream->header.type = type;
	stream->header.flags = 0;
	stream->str = nullptr;
	return stream;
}

struct EvacData {
	uint8_t** write;
	MemArena* arena;
};

inline SlangHeader* Evacuate(SlangHeader* obj,EvacData* data){
	if (!obj) return nullptr;
	if (!data->arena->InCurrSet((uint8_t*)obj)) return obj;
	
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
			func(&lam->expr,data);
			func((SlangHeader**)&lam->params,data);
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
			}
			return;
		}
		
		case SlangType::Params:
		case SlangType::Int:
		case SlangType::Real:
		case SlangType::Bool:
		case SlangType::Symbol:
		case SlangType::Storage:
		case SlangType::EndOfFile:
		case SlangType::NullType:
			return;
	}
	assert(false);
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

inline void SlangInterpreter::ReallocSet(size_t newSize){
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
	
	for (size_t i=0;i<frameIndex;++i){
		if (funcArgStack[i] && funcArgStack[i]->IsForwarded())
			funcArgStack[i] = funcArgStack[i]->GetForwardAddress();
	}
	for (size_t i=0;i<=envIndex;++i){
		if (envStack[i] && envStack[i]->header.IsForwarded())
			envStack[i] = (SlangEnv*)envStack[i]->header.GetForwardAddress();
	}
	for (size_t i=1;i<=exprIndex;++i){
		if (exprStack[i] && exprStack[i]->IsForwarded())
			exprStack[i] = exprStack[i]->GetForwardAddress();
		if (argStack[i] && argStack[i]->header.IsForwarded())
			argStack[i] = (SlangList*)argStack[i]->header.GetForwardAddress();
	}
	
	for (size_t i=0;i<finalizers.size();++i){
		auto& finalizer = finalizers[i];
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

inline void SlangInterpreter::SmallGC(size_t allocAttempt){
	uint8_t* write = arena->otherSet;
	uint8_t* read = write;
	EvacData data = {&write,arena};
	for (size_t i=0;i<frameIndex;++i){
		EvacuateOrForward(&funcArgStack[i],&data);
	}
	
	for (size_t i=0;i<=envIndex;++i){
		EvacuateOrForward((SlangHeader**)&envStack[i],&data);
	}
	
	assert(exprIndex==argIndex);
	for (size_t i=1;i<=exprIndex;++i){
		EvacuateOrForward(&exprStack[i],&data);
		EvacuateOrForward((SlangHeader**)&argStack[i],&data);
	}
	
	Scavenge(read,&data);
	
	arena->SwapSets();
	arena->currPointer = write;
	
	for (size_t i=0;i<finalizers.size();++i){
		auto& finalizer = finalizers[i];
		if (!finalizer.obj->IsForwarded()){
			std::cout << "gc finalize\n";
			finalizer.func(this,finalizer.obj);
			finalizers.erase(finalizers.begin()+i);
			--i;
		} else {
			finalizer.obj = finalizer.obj->GetForwardAddress();
			std::cout << "gc saved final\n";
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

SlangHeader* SlangInterpreter::Copy(SlangHeader* expr){
	if (!expr) return expr;
	
	StackAddArg(expr);
	size_t size = expr->GetSize();
	SlangHeader* copied = (SlangHeader*)Allocate(size);
	expr = funcArgStack[--frameIndex];
	memcpy(copied,expr,size);
	
	return copied;
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

inline SymbolName SlangInterpreter::GenSym(){
	return (genIndex++) | GEN_FLAG;
}

inline SlangObj* SlangInterpreter::MakeMaybe(SlangHeader* m,bool occupied){
	StackAddArg(m);
	SlangObj* obj = AllocateSmallObj();
	obj->header.type = SlangType::Maybe;
	if (occupied){
		obj->header.flags |= FLAG_MAYBE_OCCUPIED;
		obj->maybe = funcArgStack[frameIndex-1];
	} else {
		obj->maybe = nullptr;
	}
	--frameIndex;
	return obj;
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

inline SlangHeader* SlangInterpreter::MakeBool(bool b){
	SlangHeader* obj = (SlangHeader*)Allocate(sizeof(SlangHeader));
	obj->type = SlangType::Bool;
	obj->flags = 0;
	obj->boolVal = b;
	return obj;
}

inline SlangObj* SlangInterpreter::MakeSymbol(SymbolName symbol){
	SlangObj* obj = AllocateSmallObj();
	obj->header.type = SlangType::Symbol;
	obj->symbol = symbol;
	return obj;
}

inline SlangHeader* SlangInterpreter::MakeEOF(){
	SlangHeader* obj = (SlangHeader*)Allocate(sizeof(SlangHeader));
	obj->type = SlangType::EndOfFile;
	obj->flags = 0;
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
	#ifdef NDEBUG
		obj->env->DefSymbol(arg,nullptr);
	#else
		bool ret = obj->env->DefSymbol(arg,nullptr);
		assert(ret);
	#endif
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
	if (start==end) return nullptr;
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

inline void SlangInterpreter::ConcatLists(SlangList* a,SlangList* b) const {
	while (a->right){
		a = (SlangList*)a->right;
	}
	a->right = (SlangHeader*)b;
}

inline SlangStream* SlangInterpreter::MakeStringInputStream(SlangStr* str){
	StackAddArg((SlangHeader*)str);
	SlangStream* stream = AllocateStream(SlangType::InputStream);
	stream->header.isFile = false;
	stream->pos = 0;
	stream->str = (SlangStr*)funcArgStack[--frameIndex];
	return stream;
}

inline SlangStream* SlangInterpreter::MakeStringOutputStream(SlangStr* str){
	StackAddArg((SlangHeader*)str);
	SlangStream* stream = AllocateStream(SlangType::OutputStream);
	stream->header.isFile = false;
	if (str==nullptr){
		// create new string
		StackAddArg((SlangHeader*)stream);
		str = AllocateStr(8);
		str->storage->size = 0;
		
		stream = (SlangStream*)funcArgStack[--frameIndex];
		stream->str = str;
		stream->pos = 0;
		--frameIndex;
	} else {
		stream->str = (SlangStr*)funcArgStack[--frameIndex];
		stream->pos = GetStorageSize(str->storage);
	}
	return stream;
}

bool GCManualCollect(SlangInterpreter* s,SlangHeader** res){
	s->SmallGC(0);
	*res = nullptr;
	return true;
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
			c += GetRecursiveSize((SlangHeader*)lam->params);
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

bool GCRecObjSize(SlangInterpreter* s,SlangHeader** res){
	SlangList* arg = s->GetCurrArg();
	SlangHeader* val;
	
	if (!s->WrappedEvalExpr(arg->left,&val))
		return false;
		
	if (val==nullptr){
		*res = (SlangHeader*)s->MakeInt(0);
		return true;
	}
	
	*res = (SlangHeader*)s->MakeInt(GetRecursiveSize(val));
	return true;
}

bool GCObjSize(SlangInterpreter* s,SlangHeader** res){
	SlangList* arg = s->GetCurrArg();
	SlangHeader* val;
	
	if (!s->WrappedEvalExpr(arg->left,&val))
		return false;
		
	if (val==nullptr){
		*res = (SlangHeader*)s->MakeInt(0);
		return true;
	}
	
	size_t c = 0;
	switch (val->type){
		case SlangType::Vector:
			c = val->GetSize();
			c += ((SlangVec*)val)->storage->header.GetSize();
			break;
		case SlangType::String:
			c = val->GetSize();
			c += ((SlangStr*)val)->storage->header.GetSize();
			break;
		
		default:
			c = val->GetSize();
	}
	
	*res = (SlangHeader*)s->MakeInt(c);
	return true;
}

bool GCMemSize(SlangInterpreter* s,SlangHeader** res){
	size_t size = s->arena->currPointer - s->arena->currSet;
	*res = (SlangHeader*)s->MakeInt(size);
	return true;
}

bool GCMemCapacity(SlangInterpreter* s,SlangHeader** res){
	size_t size = s->arena->memSize/2;
	*res = (SlangHeader*)s->MakeInt(size);
	return true;
}

#define NEXT_ARG() { \
	if (!NextArg()){ \
		EvalError((SlangHeader*)GetCurrArg()); \
		return false; \
	}}
	
#define TYPE_CHECK_EXACT(obj,type) { \
	if (GetType((SlangHeader*)(obj))!=(type)){ \
		TypeError((SlangHeader*)(obj),GetType((SlangHeader*)(obj)),(type)); \
		return false; \
	}}
	
#define EXT_NEXT_ARG() { \
	if (!s->NextArg()){ \
		s->EvalError((SlangHeader*)s->GetCurrArg()); \
		return false; \
	}} \
	
#define EXT_TYPE_CHECK_EXACT(obj,type) { \
	if (GetType(obj)!=(type)){ \
		s->TypeError((obj),GetType(obj),(type)); \
		return false; \
	}}
	
bool ExtFuncGetTime(SlangInterpreter* s,SlangHeader** res){
	SlangObj* realObj = s->MakeReal(GetDoubleTime());
	*res = (SlangHeader*)realObj;
	return true;
}

bool ExtFuncGetPerfTime(SlangInterpreter* s,SlangHeader** res){
	SlangObj* realObj = s->MakeReal(GetDoublePerfTime());
	*res = (SlangHeader*)realObj;
	return true;
}

bool ExtFuncRand(SlangInterpreter* s,SlangHeader** res){
	SlangObj* intObj = s->MakeInt(gRandState.Get64());
	*res = (SlangHeader*)intObj;
	return true;
}

bool ExtFuncRandSeed(SlangInterpreter* s,SlangHeader** res){
	SlangList* argIt = s->GetCurrArg();
	
	SlangHeader* seedObj;
	if (!s->WrappedEvalExpr(argIt->left,&seedObj))
		return false;
	
	EXT_TYPE_CHECK_EXACT(seedObj,SlangType::Int);
	gRandState.Seed(((SlangObj*)seedObj)->integer);
	*res = seedObj;
	return true;
}

bool ExtFuncPrint(SlangInterpreter* s,SlangHeader** res){
	SlangList* argIt = s->GetCurrArg();
	
	SlangHeader* printObj = nullptr;
	while (argIt){
		if (!s->WrappedEvalExpr(argIt->left,&printObj)){
			s->EvalError(s->GetCurrArg()->left);
			return false;
		}
		
		if (printObj){
			std::cout << *printObj << '\n';
		} else {
			std::cout << "()\n";
		}
		
		EXT_NEXT_ARG();
		argIt = s->GetCurrArg();
	}
	
	*res = nullptr;
	return true;
}

bool ExtFuncInput(SlangInterpreter* s,SlangHeader** res){
	*res = s->SlangInputFromFile(stdin);
	return true;
}

bool ExtFuncOutput(SlangInterpreter* s,SlangHeader** res){
	SlangList* argIt = s->GetCurrArg();
	
	SlangHeader* printObj = nullptr;
	while (argIt){
		if (!s->WrappedEvalExpr(argIt->left,&printObj)){
			s->EvalError(s->GetCurrArg()->left);
			return false;
		}
		
		if (!s->SlangOutputToFile(stdout,printObj))
			return false;
			
		EXT_NEXT_ARG();
		argIt = s->GetCurrArg();
	}
	
	*res = nullptr;
	return true;
}

void ExtFileFinalizer(SlangInterpreter*,SlangHeader* obj){
	assert(obj->type==SlangType::InputStream||obj->type==SlangType::OutputStream);
	SlangStream* stream = (SlangStream*)obj;
	assert(stream->header.isFile);
	if (stream->file){
		fclose(stream->file);
		stream->file = NULL;
	}
}

bool ExtFuncFileClose(SlangInterpreter* s,SlangHeader** res){
	SlangList* argIt = s->GetCurrArg();
	
	SlangHeader* streamObj;
	if (!s->WrappedEvalExpr(argIt->left,&streamObj))
		return false;
	
	if (GetType(streamObj)!=SlangType::InputStream&&GetType(streamObj)!=SlangType::OutputStream){
		s->TypeError(s->GetCurrArg()->left,GetType(streamObj),SlangType::InputStream);
		return false;
	}
	
	if (!streamObj->isFile){
		s->StreamError(s->GetCurrArg()->left,"Stream is not a file stream!");
		return false;
	}
	
	SlangStream* stream = (SlangStream*)streamObj;
	if (stream->file){
		fclose(stream->file);
		stream->file = NULL;
	}
	
	s->RemoveFinalizer(streamObj);
	
	*res = nullptr;
	return true;
}

bool ExtFuncStdIn(SlangInterpreter* s,SlangHeader** res){
	SlangStream* stream = s->AllocateStream(SlangType::InputStream);
	stream->header.isFile = true;
	stream->pos = 0;
	stream->file = stdin;
	
	*res = (SlangHeader*)stream;
	return true;
}

bool ExtFuncStdOut(SlangInterpreter* s,SlangHeader** res){
	SlangStream* stream = s->AllocateStream(SlangType::OutputStream);
	stream->header.isFile = true;
	stream->pos = 0;
	stream->file = stdout;
	
	*res = (SlangHeader*)stream;
	return true;
}

bool ExtFuncStdErr(SlangInterpreter* s,SlangHeader** res){
	SlangStream* stream = s->AllocateStream(SlangType::OutputStream);
	stream->header.isFile = true;
	stream->pos = 0;
	stream->file = stderr;
	
	*res = (SlangHeader*)stream;
	return true;
}

bool ExtGetFilename(SlangInterpreter* s,std::string& str){
	SlangList* argIt = s->GetCurrArg();
	
	SlangHeader* strObj;
	if (!s->WrappedEvalExpr(argIt->left,&strObj))
		return false;
		
	EXT_TYPE_CHECK_EXACT(strObj,SlangType::String);
	
	SlangStr* filename = (SlangStr*)strObj;
	if (GetStorageSize(filename->storage)==0){
		s->FileError(s->GetCurrArg()->left,"Could not open file with empty filename!");
		return false;
	}
	
	str.resize(filename->storage->size);
	memcpy(str.data(),&filename->storage->data[0],sizeof(uint8_t)*filename->storage->size);
	return true;
}

bool ExtFuncFileOpenWithMode(
		SlangInterpreter* s,
		SlangHeader** res,
		const char* mode,
		SlangType streamType){
	std::string str{};
	if (!ExtGetFilename(s,str))
		return false;
	
	FILE* f = fopen(str.c_str(),mode);
	if (!f){
		std::stringstream ss{};
		ss << "Could not open path '" << str << "'";
		s->FileError(s->GetCurrArg()->left,ss.str());
		return false;
	}
	
	SlangStream* stream = s->AllocateStream(streamType);
	stream->header.isFile = true;
	stream->file = f;
	stream->pos = 0;
	
	s->AddFinalizer({(SlangHeader*)stream,&ExtFileFinalizer});
	
	*res = (SlangHeader*)stream;
	return true;
}

bool ExtFuncFileOpenRead(SlangInterpreter* s,SlangHeader** res){
	return ExtFuncFileOpenWithMode(s,res,"r",SlangType::InputStream);
}

bool ExtFuncFileOpenWrite(SlangInterpreter* s,SlangHeader** res){
	return ExtFuncFileOpenWithMode(s,res,"w",SlangType::OutputStream);
}

bool ExtFuncFileOpenWriteApp(SlangInterpreter* s,SlangHeader** res){
	return ExtFuncFileOpenWithMode(s,res,"a",SlangType::OutputStream);
}

bool ExtFuncFileFlush(SlangInterpreter* s,SlangHeader** res){
	SlangList* argIt = s->GetCurrArg();
	
	SlangHeader* streamObj;
	if (!s->WrappedEvalExpr(argIt->left,&streamObj))
		return false;
	
	if (GetType(streamObj)!=SlangType::InputStream&&GetType(streamObj)!=SlangType::OutputStream){
		s->TypeError(s->GetCurrArg()->left,GetType(streamObj),SlangType::InputStream);
		return false;
	}
	
	if (!streamObj->isFile){
		*res = streamObj;
		return true;
	}
	
	SlangStream* stream = (SlangStream*)streamObj;
	if (!stream->file){
		s->FileError(s->GetCurrArg()->left,"Cannot flush a closed file!");
		return false;
	}
	
	fflush(stream->file);
	*res = (SlangHeader*)stream;
	return true;
}

bool ExtFuncIsFile(SlangInterpreter* s,SlangHeader** res){
	SlangList* argIt = s->GetCurrArg();
	
	SlangHeader* streamObj;
	if (!s->WrappedEvalExpr(argIt->left,&streamObj))
		return false;
	
	if (GetType(streamObj)!=SlangType::InputStream&&GetType(streamObj)!=SlangType::OutputStream){
		s->TypeError(s->GetCurrArg()->left,GetType(streamObj),SlangType::InputStream);
		return false;
	}
	
	*res = (SlangHeader*)s->MakeBool(streamObj->isFile);
	return true;
}

bool ExtFuncIsFileOpen(SlangInterpreter* s,SlangHeader** res){
	SlangList* argIt = s->GetCurrArg();
	
	SlangHeader* streamObj;
	if (!s->WrappedEvalExpr(argIt->left,&streamObj))
		return false;
	
	if (GetType(streamObj)!=SlangType::InputStream&&GetType(streamObj)!=SlangType::OutputStream){
		s->TypeError(s->GetCurrArg()->left,GetType(streamObj),SlangType::InputStream);
		return false;
	}
	
	if (!streamObj->isFile){
		s->StreamError(s->GetCurrArg()->left,"Stream is not a file stream!");
		return false;
	}
	
	*res = (SlangHeader*)s->MakeBool(((SlangStream*)streamObj)->file!=NULL);
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
inline void SlangInterpreter::DefEnvSymbol(SlangEnv* e,SymbolName name,SlangHeader* val){
	StackAddArg(val);
	if (!e->DefSymbol(name,val)){
		SlangEnv* newEnv = AllocateEnv(1);
		newEnv->DefSymbol(name,funcArgStack[frameIndex-1]);
		e = GetCurrEnv();
		e->AddBlock(newEnv);
	}
	--frameIndex;
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

inline bool SlangInterpreter::SlangOutputToFile(FILE* file,SlangHeader* obj){
	switch (GetType(obj)){
		case SlangType::String: {
			SlangStr* str = (SlangStr*)obj;
			for (size_t i=0;i<GetStorageSize(str->storage);++i){
				fputc(str->storage->data[i],file);
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
			std::string symStr = parser.GetSymbolString(symObj->symbol);
			fprintf(file,"%s",symStr.c_str());
			break;
		}
		case SlangType::EndOfFile:
			break;
		default:
			TypeError(obj,GetType(obj),SlangType::String);
			return false;
	}
	return true;
}

inline SlangHeader* SlangInterpreter::SlangInputFromFile(FILE* file){
	std::string temp{};
	temp.reserve(256);
	int c;
	while ((c = fgetc(file))!=EOF){
		if (c=='\n') break;
		temp.push_back(c);
	}
	
	if (temp.empty()&&c==EOF){
		return MakeEOF();
	}
	
	SlangStr* str = AllocateStr(temp.size());
	if (temp.empty()) return (SlangHeader*)str;
	memcpy(&str->storage->data[0],temp.data(),temp.size());
	return (SlangHeader*)str;
}

inline bool SlangInterpreter::SlangOutputToString(SlangStream* stream,SlangHeader* obj){
	static char numChars[64];
	
	SlangStr* sstr = stream->str;
	switch (GetType(obj)){
		case SlangType::String: {
			SlangStr* str = (SlangStr*)obj;
			if (GetStorageSize(str->storage)==0) break;
			
			StackAddArg((SlangHeader*)str);
			stream = ReallocateStream(stream,str->storage->size);
			sstr = stream->str;
			str = (SlangStr*)funcArgStack[--frameIndex];
			// stream is now guaranteed big enough
			
			uint8_t* it = sstr->storage->data+stream->pos;
			uint8_t* otherIt = str->storage->data;
			for (size_t i=0;i<str->storage->size;++i){
				*it++ = *otherIt++;
			}
			
			stream->pos += str->storage->size;
			break;
		}
		case SlangType::Int: {
			SlangObj* intObj = (SlangObj*)obj;
			ssize_t size = snprintf(numChars,sizeof(numChars),"%ld",intObj->integer);
			assert(size!=-1);
			stream = ReallocateStream(stream,size);
			sstr = stream->str;
			
			uint8_t* it = sstr->storage->data+stream->pos;
			uint8_t* otherIt = (uint8_t*)&numChars[0];
			for (ssize_t i=0;i<size;++i){
				*it++ = *otherIt++;
			}
			
			stream->pos += size;
			break;
		}
		case SlangType::Real: {
			SlangObj* realObj = (SlangObj*)obj;
			ssize_t size = snprintf(numChars,sizeof(numChars),"%.16g",realObj->real);
			assert(size!=-1);
			stream = ReallocateStream(stream,size);
			sstr = stream->str;
			
			uint8_t* it = sstr->storage->data+stream->pos;
			uint8_t* otherIt = (uint8_t*)&numChars[0];
			for (ssize_t i=0;i<size;++i){
				*it++ = *otherIt++;
			}
			
			stream->pos += size;
			break;
		}
		case SlangType::Symbol: {
			SlangObj* symObj = (SlangObj*)obj;
			std::string symStr = parser.GetSymbolString(symObj->symbol);
			ssize_t size = symStr.size();
			stream = ReallocateStream(stream,size);
			sstr = stream->str;
			
			uint8_t* it = sstr->storage->data+stream->pos;
			uint8_t* otherIt = (uint8_t*)symStr.data();
			for (ssize_t i=0;i<size;++i){
				*it++ = *otherIt++;
			}
			
			stream->pos += size;
			break;
		}
		case SlangType::EndOfFile:
			break;
		default:
			TypeError(obj,GetType(obj),SlangType::String);
			return false;
	}
	return true;
}

inline SlangHeader* SlangInterpreter::SlangInputFromString(SlangStream* stream){
	std::string temp{};
	temp.reserve(256);
	uint8_t c;
	size_t strSize = GetStorageSize(stream->str->storage);
	if (!strSize||stream->pos>=strSize){
		return MakeEOF();
	}
	
	while (true){
		c = stream->str->storage->data[stream->pos++];
		if (c=='\n')
			break;
		temp.push_back(c);
		if (stream->pos>=strSize) break;
	}
	
	if (temp.empty()&&stream->pos>=strSize){
		return MakeEOF();
	}
	
	SlangStr* str = AllocateStr(temp.size());
	if (temp.empty()) return (SlangHeader*)str;
	memcpy(&str->storage->data[0],temp.data(),temp.size());
	return (SlangHeader*)str;
}

inline bool SlangInterpreter::SlangFuncDefine(SlangHeader** res){
	// can't exist
	SlangList* argIt = GetCurrArg();
	if (GetType(argIt->left)!=SlangType::Symbol){
		TypeError(argIt->left,GetType(argIt->left),SlangType::Symbol);
		return false;
	}
	SlangObj* symObj = (SlangObj*)argIt->left;
	SymbolName sym = symObj->symbol;
	SlangHeader* tmp;
	bool redefined = GetCurrEnv()->GetSymbol(sym,&tmp);
	if (GetCurrEnv()->parent!=nullptr&&redefined){
		RedefinedError((SlangHeader*)argIt,sym);
		return false;
	}
	
	NEXT_ARG();
	
	SlangHeader* valObj = nullptr;
	if (!WrappedEvalExpr(GetCurrArg()->left,&valObj))
		return false;
	
	if (redefined){
		GetCurrEnv()->SetSymbol(sym,valObj);
	} else {
		DefEnvSymbol(GetCurrEnv(),sym,valObj);
	}
	*res = valObj;
	return true;
}

inline bool SlangInterpreter::SlangFuncLambda(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	if (!IsList(argIt->left)&&GetType(argIt->left)!=SlangType::Symbol){
		std::stringstream msg = {};
		msg << "LambdaError:\n    Expected parameter list, not '" << *argIt->left << "'";
		PushError(argIt->left,msg.str());
		return false;
	}
	
	bool variadic = GetType(argIt->left)==SlangType::Symbol;
	
	std::vector<SymbolName> params = {};
	params.reserve(5);
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
			std::stringstream msg = {};
			msg << "LambdaError:\n    Lambda parameter '" << 
				*(SlangHeader*)symObj << "' was reused!";
			PushError((SlangHeader*)symObj,msg.str());
			return false;
		}
		
		params.push_back(symObj->symbol);
		seen.insert(symObj->symbol);
		
		if (variadic) break;
		paramIt = (SlangList*)paramIt->right;
	}
	NEXT_ARG();
	argIt = GetCurrArg();

	SlangLambda* lambdaObj = MakeLambda(argIt->left,params,variadic);
	*res = (SlangHeader*)lambdaObj;
	return true;
}

inline bool SlangInterpreter::SlangFuncSet(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	if (GetType(argIt->left)!=SlangType::Symbol){
		TypeError(argIt->left,GetType(argIt->left),SlangType::Symbol);
		return false;
	}
	
	SymbolName sym = ((SlangObj*)argIt->left)->symbol;
	NEXT_ARG();
	argIt = GetCurrArg();
	
	SlangHeader* valObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&valObj))
		return false;
		
	if (!SetRecursiveSymbol(GetCurrEnv(),sym,valObj)){
		std::stringstream msg = {};
		msg << "SetError:\n    Cannot set undefined symbol '" << parser.GetSymbolString(sym) << "'";
		PushError(GetCurrArg()->left,msg.str());
		return false;
	}
	
	*res = valObj;
	return true;
}

inline bool SlangInterpreter::SlangFuncQuote(SlangHeader** res){
	*res = Copy(GetCurrArg()->left);
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
		case SlangType::EndOfFile:
			res = false;
			return true;
		case SlangType::Bool:
			res = obj->boolVal;
			return true;
		case SlangType::Int:
			res = (bool)((SlangObj*)obj)->integer;
			return true;
		case SlangType::Real:
			res = (bool)((SlangObj*)obj)->real;
			return true;
		case SlangType::Maybe:
			res = (obj->flags&FLAG_MAYBE_OCCUPIED)!=0;
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
		case SlangType::InputStream: {
			SlangStream* stream = (SlangStream*)obj;
			res = !stream->IsAtEnd();
			return true;
		}
		case SlangType::OutputStream:
			res = true;
			return true;
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

inline bool SlangInterpreter::SlangFuncNot(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
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

inline bool SlangInterpreter::SlangFuncNegate(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
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

inline bool SlangInterpreter::SlangFuncLen(SlangHeader** res){
	SlangHeader* v;
	if (!WrappedEvalExpr(GetCurrArg()->left,&v))
		return false;
	
	SlangType t = GetType(v);
	switch (t){
		case SlangType::NullType:
			*res = (SlangHeader*)MakeInt(0);
			return true;
		case SlangType::List:
			*res = (SlangHeader*)MakeInt(ListLen((SlangList*)v));
			return true;
		case SlangType::Vector:
			*res = (SlangHeader*)MakeInt(GetStorageSize(((SlangVec*)v)->storage));
			return true;
		case SlangType::String:
			*res = (SlangHeader*)MakeInt(GetStorageSize(((SlangStr*)v)->storage));
			return true;
		default:
			TypeError(v,t,SlangType::List);
			return false;
	}
}

inline bool SlangInterpreter::SlangFuncUnwrap(SlangHeader** res){
	SlangList* arg = GetCurrArg();
	SlangHeader* r;
	if (!WrappedEvalExpr(arg->left,&r))
		return false;
		
	if (GetType(r)!=SlangType::Maybe){
		TypeError(r,GetType(r),SlangType::Maybe);
		return false;
	}
	
	if (!(r->flags & FLAG_MAYBE_OCCUPIED)){
		UnwrapError(GetCurrArg()->left);
		return false;
	}
	
	*res = ((SlangObj*)r)->maybe;
	return true;
}

inline bool SlangInterpreter::SlangFuncList(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
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
		NEXT_ARG();
		argIt = GetCurrArg();
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

inline bool SlangInterpreter::SlangFuncVec(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
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
		NEXT_ARG();
		argIt = GetCurrArg();
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

inline bool SlangInterpreter::SlangFuncVecAlloc(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* sizeObj;
	if (!WrappedEvalExpr(argIt->left,&sizeObj))
		return false;
	
	if (GetType(sizeObj)!=SlangType::Int){
		TypeError(sizeObj,GetType(sizeObj),SlangType::Int);
		return false;
	}
	size_t size = ((SlangObj*)sizeObj)->integer;
	
	argIt = GetCurrArg();
	SlangHeader* fill = nullptr;
	if (argIt->right){
		NEXT_ARG();
		argIt = GetCurrArg();
		if (!WrappedEvalExpr(argIt->left,&fill))
			return false;
	}
	
	SlangVec* vecObj = AllocateVec(size,fill);
	*res = (SlangHeader*)vecObj;
	return true;
}

inline bool SlangInterpreter::SlangFuncVecGet(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* vecObj;
	if (!WrappedEvalExpr(argIt->left,&vecObj))
		return false;
	
	if (GetType(vecObj)!=SlangType::Vector){
		TypeError(vecObj,GetType(vecObj),SlangType::Vector);
		return false;
	}
	
	SlangVec* vec = (SlangVec*)vecObj;
	StackAddArg((SlangHeader*)vec);
	
	NEXT_ARG();
	argIt = GetCurrArg();

	SlangHeader* indexObj;
	if (!WrappedEvalExpr(argIt->left,&indexObj))
		return false;
	
	vec = (SlangVec*)funcArgStack[frameIndex-1];
	if (GetType(indexObj)!=SlangType::Int){
		TypeError(indexObj,GetType(indexObj),SlangType::Int);
		return false;
	}
	ssize_t index = ((SlangObj*)indexObj)->integer;
	size_t vecLen = GetStorageSize(vec->storage);
	if (index<0) index += vecLen;
	if (index>=(ssize_t)vecLen||index<0){
		if (index<0) index -= vecLen;
		IndexError(GetCurrArg()->left,vecLen,index);
		return false;
	}
	
	vec = (SlangVec*)funcArgStack[--frameIndex];
	*res = vec->storage->objs[index];
	return true;
}

inline bool SlangInterpreter::SlangFuncVecSet(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* vecObj;
	if (!WrappedEvalExpr(argIt->left,&vecObj))
		return false;
	
	if (GetType(vecObj)!=SlangType::Vector){
		TypeError(vecObj,GetType(vecObj),SlangType::Vector);
		return false;
	}
	
	SlangVec* vec = (SlangVec*)vecObj;
	StackAddArg((SlangHeader*)vec);
	
	NEXT_ARG();
	argIt = GetCurrArg();

	SlangHeader* indexObj;
	if (!WrappedEvalExpr(argIt->left,&indexObj))
		return false;
	
	if (GetType(indexObj)!=SlangType::Int){
		TypeError(indexObj,GetType(indexObj),SlangType::Int);
		return false;
	}
	ssize_t index = ((SlangObj*)indexObj)->integer;
	size_t vecLen = GetStorageSize(vec->storage);
	if (index<0) index += vecLen;
	if (index>=(ssize_t)vecLen||index<0){
		if (index<0) index -= vecLen;
		IndexError(GetCurrArg()->left,vecLen,index);
		return false;
	}
	
	NEXT_ARG();
	argIt = GetCurrArg();
	SlangHeader* val;
	if (!WrappedEvalExpr(argIt->left,&val))
		return false;
	
	vec = (SlangVec*)funcArgStack[--frameIndex];
	vec->storage->objs[index] = val;
	*res = (SlangHeader*)vec;
	return true;
}

inline bool SlangInterpreter::SlangFuncVecAppend(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* vecObj;
	if (!WrappedEvalExpr(argIt->left,&vecObj))
		return false;
	
	if (GetType(vecObj)!=SlangType::Vector){
		TypeError(vecObj,GetType(vecObj),SlangType::Vector);
		return false;
	}
	
	StackAddArg(vecObj);
	
	NEXT_ARG();
	argIt = GetCurrArg();
	SlangHeader* val;
	if (!WrappedEvalExpr(argIt->left,&val))
		return false;
		
	StackAddArg(val);
	
	SlangVec* vec = (SlangVec*)funcArgStack[frameIndex-2];
	if (!vec->storage){
		SlangStorage* storage = AllocateStorage(4,sizeof(SlangHeader*),nullptr);
		vec = (SlangVec*)funcArgStack[frameIndex-2];
		vec->storage = storage;
		vec->storage->size = 0;
	} else if (vec->storage->capacity<vec->storage->size+1){
		SlangStorage* storage = AllocateStorage(vec->storage->capacity*5/4+1,sizeof(SlangHeader*),nullptr);
		vec = (SlangVec*)funcArgStack[frameIndex-2];
		storage->size = vec->storage->size;
		memcpy(storage->objs,vec->storage->objs,sizeof(SlangHeader*)*vec->storage->size);
		vec->storage = storage;
	}
	vec->storage->objs[vec->storage->size++] = funcArgStack[frameIndex-1];
	
	frameIndex -= 2;
	*res = (SlangHeader*)vec;
	
	return true;
}

inline bool SlangInterpreter::SlangFuncVecPop(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* vecObj;
	if (!WrappedEvalExpr(argIt->left,&vecObj))
		return false;
	
	if (GetType(vecObj)!=SlangType::Vector){
		TypeError(vecObj,GetType(vecObj),SlangType::Vector);
		return false;
	}
	
	SlangVec* vec = (SlangVec*)vecObj;
	
	if (GetStorageSize(vec->storage)<1){
		ValueError(GetCurrArg()->left,"Popped from an empty vector!");
		return false;
	}
	
	SlangHeader* popObj = vec->storage->objs[--vec->storage->size];
	if (vec->storage->size<vec->storage->capacity*5/8 && vec->storage->capacity>4){
		vec->storage->capacity = vec->storage->size*5/4+1;
	}
	
	*res = popObj;
	return true;
}

inline bool SlangInterpreter::SlangFuncStrGet(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* strObj;
	if (!WrappedEvalExpr(argIt->left,&strObj))
		return false;
	
	TYPE_CHECK_EXACT(strObj,SlangType::String);
	
	SlangStr* str = (SlangStr*)strObj;
	StackAddArg((SlangHeader*)str);
	
	NEXT_ARG();
	argIt = GetCurrArg();

	SlangHeader* indexObj;
	if (!WrappedEvalExpr(argIt->left,&indexObj))
		return false;
	
	TYPE_CHECK_EXACT(indexObj,SlangType::Int);
	
	str = (SlangStr*)funcArgStack[frameIndex-1];
	ssize_t index = ((SlangObj*)indexObj)->integer;
	size_t strLen = GetStorageSize(str->storage);
	if (index<0) index += strLen;
	if (index>=(ssize_t)strLen||index<0){
		if (index<0) index -= strLen;
		IndexError(GetCurrArg()->left,strLen,index);
		return false;
	}
	
	SlangStr* newStr = AllocateStr(1);
	str = (SlangStr*)funcArgStack[--frameIndex];
	newStr->storage->data[0] = str->storage->data[index];
	*res = (SlangHeader*)newStr;
	return true;
}

inline bool SlangInterpreter::SlangFuncStrSet(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* strObj;
	if (!WrappedEvalExpr(argIt->left,&strObj))
		return false;
	
	TYPE_CHECK_EXACT(strObj,SlangType::String);
	
	SlangStr* str = (SlangStr*)strObj;
	StackAddArg((SlangHeader*)str);
	
	NEXT_ARG();
	argIt = GetCurrArg();
	SlangHeader* indexObj;
	if (!WrappedEvalExpr(argIt->left,&indexObj))
		return false;
		
	TYPE_CHECK_EXACT(indexObj,SlangType::Int);
	
	str = (SlangStr*)funcArgStack[frameIndex-1];
	ssize_t index = ((SlangObj*)indexObj)->integer;
	size_t strLen = GetStorageSize(str->storage);
	if (index<0) index += strLen;
	if (index>=(ssize_t)strLen||index<0){
		if (index<0) index -= strLen;
		IndexError(GetCurrArg()->left,strLen,index);
		return false;
	}
	
	NEXT_ARG();
	argIt = GetCurrArg();
	SlangHeader* val;
	if (!WrappedEvalExpr(argIt->left,&val))
		return false;
		
	TYPE_CHECK_EXACT(val,SlangType::String);
	
	size_t valLen = GetStorageSize(((SlangStr*)val)->storage);
	if (valLen!=1){
		ValueError(GetCurrArg()->left,"Expected character, not string of length "+std::to_string(valLen));
		return false;
	}
	
	str = (SlangStr*)funcArgStack[--frameIndex];
	str->storage->data[index] = ((SlangStr*)val)->storage->data[0];
	*res = (SlangHeader*)str;
	return true;
}

inline bool SlangInterpreter::SlangFuncMakeStrIStream(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	
	SlangHeader* strObj;
	if (!WrappedEvalExpr(argIt->left,&strObj))
		return false;
		
	TYPE_CHECK_EXACT(strObj,SlangType::String);
	
	SlangStr* str = (SlangStr*)strObj;
	
	SlangStream* stream = MakeStringInputStream(str);
	*res = (SlangHeader*)stream;
	
	return true;
}

inline bool SlangInterpreter::SlangFuncMakeStrOStream(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangStream* stream;
	if (!argIt){
		stream = MakeStringOutputStream(nullptr);
	} else {
		SlangHeader* strObj;
		if (!WrappedEvalExpr(argIt->left,&strObj))
			return false;
			
		TYPE_CHECK_EXACT(strObj,SlangType::String);
		
		stream = MakeStringOutputStream((SlangStr*)strObj);
	}
	
	*res = (SlangHeader*)stream;
	return true;
}

inline bool SlangInterpreter::SlangFuncStreamGetStr(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* streamObj;
	if (!WrappedEvalExpr(argIt->left,&streamObj))
		return false;
	
	SlangType type = GetType(streamObj);
	if (type!=SlangType::InputStream&&type!=SlangType::OutputStream){
		TypeError(streamObj,type,SlangType::InputStream);
		return false;
	}
	
	if (streamObj->isFile){
		StreamError(streamObj,"Stream has no underlying string!");
		return false;
	}
	
	SlangStream* stream = (SlangStream*)streamObj;
	*res = (SlangHeader*)stream->str;
	return true;
}

inline bool SlangInterpreter::SlangFuncStreamWrite(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* streamObj;
	if (!WrappedEvalExpr(argIt->left,&streamObj))
		return false;
	
	TYPE_CHECK_EXACT(streamObj,SlangType::OutputStream);
	StackAddArg(streamObj);
	
	NEXT_ARG();
	argIt = GetCurrArg();
	
	SlangHeader* strObj;
	if (!WrappedEvalExpr(argIt->left,&strObj))
		return false;
		
	TYPE_CHECK_EXACT(strObj,SlangType::String);
	if (GetStorageSize(((SlangStr*)strObj)->storage)==0){
		*res = funcArgStack[--frameIndex];
		return true;
	}
	
	SlangStream* stream = (SlangStream*)funcArgStack[frameIndex-1];
	SlangStr* str = (SlangStr*)strObj;
	if (stream->header.isFile){
		if (!stream->file){
			FileError(GetCurrArg()->left,"Cannot write to a closed file!");
			--frameIndex;
			return false;
		}
		fwrite(&str->storage->data[0],sizeof(uint8_t),str->storage->size,stream->file);
	} else {
		if (!SlangOutputToString(stream,strObj)){
			--frameIndex;
			return false;
		}
		
		/*size_t currCap = GetStorageCapacity(streamstr->storage);
		size_t currSize = GetStorageSize(streamstr->storage);
		size_t sizeIncrease;
		if (currSize-stream->pos>str->storage->size){
			sizeIncrease = 0;
		} else {
			sizeIncrease = str->storage->size-(currSize-stream->pos);
		}
		
		// needs realloc
		if (currCap-currSize<sizeIncrease){
			StackAddArg((SlangHeader*)stream);
			StackAddArg((SlangHeader*)str);
			size_t newCap = sizeIncrease+GetStorageSize(streamstr->storage);
			newCap = newCap*3/2+8;
			SlangStorage* newStorage = AllocateStorage(newCap,sizeof(uint8_t),nullptr);
			
			// fix pointers
			str = (SlangStr*)funcArgStack[--frameIndex];
			stream = (SlangStream*)funcArgStack[--frameIndex];
			streamstr = stream->str;
			
			newStorage->size = streamstr->storage->size;
			memcpy(newStorage->data,streamstr->storage->data,sizeof(uint8_t)*streamstr->storage->size);
			streamstr->storage = newStorage;
		}*/
		
		// copy data over
		/*uint8_t* it = &streamstr->storage->data[stream->pos];
		uint8_t* otherIt = &str->storage->data[0];
		uint8_t* end = it+str->storage->size;
		while (it<end){
			*it++ = *otherIt++;
		}
		streamstr->storage->size += sizeIncrease;
		stream->pos += str->storage->size;*/
	}
	
	*res = funcArgStack[--frameIndex];
	return true;
}

inline bool SlangInterpreter::SlangFuncStreamRead(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* streamObj;
	if (!WrappedEvalExpr(argIt->left,&streamObj))
		return false;
	
	TYPE_CHECK_EXACT(streamObj,SlangType::InputStream);
	bool isFile = streamObj->isFile;
	
	StackAddArg(streamObj);
	NEXT_ARG();
	argIt = GetCurrArg();
	size_t count = UINT64_MAX;
	if (argIt){
		SlangHeader* intObj;
		if (!WrappedEvalExpr(argIt->left,&intObj))
			return false;
			
		TYPE_CHECK_EXACT(intObj,SlangType::Int);
		ssize_t c = ((SlangObj*)intObj)->integer;
		count = (c < 0) ? 0 : c;
	}
	
	if (isFile){
		SlangStream* stream = (SlangStream*)funcArgStack[--frameIndex];
		if (!stream->file){
			FileError(GetCurrArg()->left,"Cannot read from a closed file!");
			return false;
		}
		
		std::string temp{};
		temp.reserve(64);
		int c=0;
		while (count--&&(c = fgetc(stream->file))!=EOF){
			temp.push_back(c);
		}
		
		if (temp.empty()){
			*res = MakeEOF();
			--frameIndex;
			return true;
		}
		
		SlangStr* intoStr = AllocateStr(temp.size());
		memcpy(&intoStr->storage->data[0],temp.data(),temp.size());
		*res = (SlangHeader*)intoStr;
	} else {
		SlangStream* stream = (SlangStream*)funcArgStack[frameIndex-1];
		size_t bytesLeft = GetStorageSize(stream->str->storage)-stream->pos;
		if (bytesLeft==0){
			*res = MakeEOF();
			--frameIndex;
			return true;
		}
		count = (count>bytesLeft) ? bytesLeft : count;
		
		SlangStr* intoStr = AllocateStr(count);
		stream = (SlangStream*)funcArgStack[--frameIndex];
		
		uint8_t* it = &intoStr->storage->data[0];
		uint8_t* otherIt = (stream->str->storage) ? &stream->str->storage->data[stream->pos] : nullptr;
		
		stream->pos += count;
		while (count-->0){
			*it++ = *otherIt++;
		}
		
		*res = (SlangHeader*)intoStr;
	}
	return true;
}

inline bool SlangInterpreter::SlangFuncStreamWriteByte(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* streamObj;
	if (!WrappedEvalExpr(argIt->left,&streamObj))
		return false;
	
	TYPE_CHECK_EXACT(streamObj,SlangType::OutputStream);
	
	NEXT_ARG();
	argIt = GetCurrArg();
	
	StackAddArg(streamObj);
	
	SlangHeader* wObj;
	if (!WrappedEvalExpr(argIt->left,&wObj))
		return false;
	
	TYPE_CHECK_EXACT(wObj,SlangType::Int);
	
	uint8_t byte = ((SlangObj*)wObj)->integer&0xFF;
	
	SlangStream* stream = (SlangStream*)funcArgStack[--frameIndex];
	if (streamObj->isFile){
		if (!stream->file){
			FileError(GetCurrArg()->left,"Cannot write to a closed file!");
			return false;
		}
		fputc(byte,stream->file);
	} else {
		SlangStr* streamstr = stream->str;
		size_t currCap = GetStorageCapacity(streamstr->storage);
		
		// needs realloc
		if (currCap==stream->pos){
			StackAddArg((SlangHeader*)stream);
			size_t newCap = 1+GetStorageSize(streamstr->storage);
			newCap = newCap*3/2+8;
			SlangStorage* newStorage = AllocateStorage(newCap,sizeof(uint8_t),nullptr);
			
			// fix pointers
			stream = (SlangStream*)funcArgStack[--frameIndex];
			streamstr = stream->str;
			
			newStorage->size = streamstr->storage->size;
			memcpy(newStorage->data,streamstr->storage->data,sizeof(uint8_t)*streamstr->storage->size);
			streamstr->storage = newStorage;
		}
		
		if (stream->pos==streamstr->storage->size)
			streamstr->storage->size++;
		streamstr->storage->data[stream->pos++] = byte;
	}
	
	*res = (SlangHeader*)stream;
	return true;
}

inline bool SlangInterpreter::SlangFuncStreamReadByte(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* streamObj;
	if (!WrappedEvalExpr(argIt->left,&streamObj))
		return false;
	
	TYPE_CHECK_EXACT(streamObj,SlangType::InputStream);
	bool isFile = streamObj->isFile;
	
	SlangStream* stream = (SlangStream*)streamObj;
	if (isFile){
		if (!stream->file){
			FileError(GetCurrArg()->left,"Cannot read from a closed file!");
			return false;
		}
		int c = fgetc(stream->file);
		if (c==EOF){
			*res = MakeEOF();
			return true;
		}
		*res = (SlangHeader*)MakeInt(c&0xFF);
	} else {
		size_t bytesLeft = GetStorageSize(stream->str->storage)-stream->pos;
		if (bytesLeft==0){
			*res = MakeEOF();
			return true;
		}
		
		uint8_t byte = stream->str->storage->data[stream->pos++];
		
		*res = (SlangHeader*)MakeInt(byte);
	}
	return true;
}

inline bool SlangInterpreter::SlangFuncStreamSeekPrefix(SlangStream** stream,ssize_t* offset){
	SlangList* argIt = GetCurrArg();
	SlangHeader* streamObj;
	if (!WrappedEvalExpr(argIt->left,&streamObj))
		return false;
	
	if (GetType(streamObj)!=SlangType::InputStream&&GetType(streamObj)!=SlangType::OutputStream){
		TypeError(GetCurrArg()->left,GetType(streamObj),SlangType::InputStream);
		return false;
	}
	
	StackAddArg(streamObj);
	
	*offset = 0;
	NEXT_ARG();
	argIt = GetCurrArg();
	if (argIt){
		SlangHeader* intObj;
		if (!WrappedEvalExpr(argIt->left,&intObj))
			return false;
		TYPE_CHECK_EXACT(intObj,SlangType::Int);
		*offset = ((SlangObj*)intObj)->integer;
	}
	
	*stream = (SlangStream*)funcArgStack[--frameIndex];
	return true;
}

inline bool SlangInterpreter::SlangFuncStreamSeekBegin(SlangHeader** res){
	SlangStream* stream;
	ssize_t offset;
	if (!SlangFuncStreamSeekPrefix(&stream,&offset))
		return false;
		
	if (stream->header.isFile){
		if (!stream->file){
			FileError(GetCurrArg()->left,"Cannot seek in a closed file!");
			return false;
		}
		fseek(stream->file,offset,SEEK_SET);
	} else {
		stream->pos = (offset<0) ? 0 : offset;
		if (stream->pos > GetStorageSize(stream->str->storage)){
			stream->pos = GetStorageSize(stream->str->storage);
		}
	}
	*res = (SlangHeader*)stream;
	return true;
}

inline bool SlangInterpreter::SlangFuncStreamSeekEnd(SlangHeader** res){
	SlangStream* stream;
	ssize_t offset;
	if (!SlangFuncStreamSeekPrefix(&stream,&offset))
		return false;
		
	if (stream->header.isFile){
		if (!stream->file){
			FileError(GetCurrArg()->left,"Cannot seek in a closed file!");
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
		if (stream->pos > GetStorageSize(stream->str->storage)){
			stream->pos = GetStorageSize(stream->str->storage);
		}
	}
	*res = (SlangHeader*)stream;
	return true;
}

inline bool SlangInterpreter::SlangFuncStreamSeekOffset(SlangHeader** res){
	SlangStream* stream;
	ssize_t offset;
	if (!SlangFuncStreamSeekPrefix(&stream,&offset))
		return false;
		
	if (stream->header.isFile){
		if (!stream->file){
			FileError(GetCurrArg()->left,"Cannot seek in a closed file!");
			return false;
		}
		fseek(stream->file,offset,SEEK_CUR);
	} else {
		ssize_t ssize = GetStorageSize(stream->str->storage);
		if (offset>(ssize_t)(ssize-stream->pos)){
			offset = ssize-stream->pos;
		} else if (offset<-(ssize_t)stream->pos){
			offset = -stream->pos;
		}
		stream->pos += offset;
	}
	*res = (SlangHeader*)stream;
	return true;
}

inline bool SlangInterpreter::SlangFuncStreamTell(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* streamObj;
	if (!WrappedEvalExpr(argIt->left,&streamObj))
		return false;
	
	if (GetType(streamObj)!=SlangType::InputStream&&GetType(streamObj)!=SlangType::OutputStream){
		TypeError(GetCurrArg()->left,GetType(streamObj),SlangType::InputStream);
		return false;
	}
	
	SlangStream* stream = (SlangStream*)streamObj;
	ssize_t t;
	if (stream->header.isFile){
		if (!stream->file){
			FileError(GetCurrArg()->left,"Cannot tell position of a closed file!");
			return false;
		}
		t = ftell(stream->file);
	} else {
		t = stream->pos;
	}
	*res = (SlangHeader*)MakeInt(t);
	return true;
}

inline bool SlangInterpreter::SlangFuncOutputTo(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* streamObj;
	if (!WrappedEvalExpr(argIt->left,&streamObj))
		return false;
		
	TYPE_CHECK_EXACT(streamObj,SlangType::OutputStream);
	
	NEXT_ARG();
	argIt = GetCurrArg();
	SlangStream* stream = (SlangStream*)streamObj;
	SlangHeader* obj;
	StackAddArg((SlangHeader*)stream);
	if (streamObj->isFile){
		if (!stream->file){
			FileError(GetCurrArg()->left,"Cannot write to a closed file!");
			return false;
		}
		while (argIt){
			if (!WrappedEvalExpr(argIt->left,&obj)){
				--frameIndex;
				return false;
			}
				
			stream = (SlangStream*)funcArgStack[frameIndex-1];
			if (!SlangOutputToFile(stream->file,obj)){
				--frameIndex;
				return false;
			}
			
			NEXT_ARG();
			argIt = GetCurrArg();
		}
	} else {
		while (argIt){
			if (!WrappedEvalExpr(argIt->left,&obj)){
				--frameIndex;
				return false;
			}
				
			stream = (SlangStream*)funcArgStack[frameIndex-1];
			if (!SlangOutputToString(stream,obj)){
				--frameIndex;
				return false;
			}
			
			NEXT_ARG();
			argIt = GetCurrArg();
		}
	}
	
	*res = funcArgStack[--frameIndex];
	
	return true;
}

inline bool SlangInterpreter::SlangFuncInputFrom(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* streamObj;
	if (!WrappedEvalExpr(argIt->left,&streamObj))
		return false;
		
	TYPE_CHECK_EXACT(streamObj,SlangType::InputStream);
	
	SlangStream* stream = (SlangStream*)streamObj;
	if (streamObj->isFile){
		if (!stream->file){
			FileError(GetCurrArg()->left,"Cannot read from a closed file!");
			return false;
		}
		*res = SlangInputFromFile(stream->file);
	} else {
		*res = SlangInputFromString(stream);
	}
	return true;
}

inline bool SlangInterpreter::SlangFuncPair(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangList* pairObj = AllocateList();
	// for GC
	StackAddArg((SlangHeader*)pairObj);
	
	pairObj->left = nullptr;
	pairObj->right = nullptr;
	
	SlangHeader* left = nullptr;
	if (!WrappedEvalExpr(argIt->left,&left))
		return false;
		
	((SlangList*)funcArgStack[frameIndex-1])->left = left;
	
	NEXT_ARG();
	argIt = GetCurrArg();
	
	SlangHeader* right = nullptr;
	if (!WrappedEvalExpr(argIt->left,&right))
		return false;
	
	((SlangList*)funcArgStack[frameIndex-1])->right = right;
	
	*res = funcArgStack[--frameIndex];
	return true;
}

inline bool SlangInterpreter::SlangFuncLeft(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* pairObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&pairObj))
		return false;
		
	if (GetType(pairObj)!=SlangType::List){
		TypeError(GetCurrArg()->left,GetType(pairObj),SlangType::List);
		return false;
	}
	
	SlangList* pair = (SlangList*)pairObj;
	*res = pair->left;
	return true;
}

inline bool SlangInterpreter::SlangFuncRight(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* pairObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&pairObj))
		return false;
	
	if (GetType(pairObj)!=SlangType::List){
		TypeError(GetCurrArg()->left,GetType(pairObj),SlangType::List);
		return false;
	}
	
	SlangList* pair = (SlangList*)pairObj;
	*res = pair->right;
	return true;
}

inline bool SlangInterpreter::SlangFuncSetLeft(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* pairObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&pairObj))
		return false;
		
	if (GetType(pairObj)!=SlangType::List){
		TypeError(GetCurrArg()->left,GetType(pairObj),SlangType::List);
		return false;
	}
	
	if (!arena->InCurrSet((uint8_t*)pairObj)){
		PushError(pairObj,"SetError:\n    Cannot set left of const data!");
		return false;
	}
	
	SlangList* pair = (SlangList*)pairObj;
	
	NEXT_ARG();
	argIt = GetCurrArg();
	
	SlangHeader* valObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&valObj))
		return false;
	
	pair->left = valObj;
	*res = (SlangHeader*)pair;
	return true;
}

inline bool SlangInterpreter::SlangFuncSetRight(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* pairObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&pairObj))
		return false;
	
	if (GetType(pairObj)!=SlangType::List){
		TypeError(GetCurrArg()->left,GetType(pairObj),SlangType::List);
		return false;
	}
	
	if (!arena->InCurrSet((uint8_t*)pairObj)){
		PushError(pairObj,"SetError:\n    Cannot set right of const data!");
		return false;
	}
	
	SlangList* pair = (SlangList*)pairObj;
	
	NEXT_ARG();
	argIt = GetCurrArg();
	
	SlangHeader* valObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&valObj))
		return false;
	
	pair->right = valObj;
	*res = (SlangHeader*)pair;
	return true;
}

inline bool SlangInterpreter::SlangFuncInc(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* numObj;
	if (!WrappedEvalExpr(argIt->left,&numObj))
		return false;
	
	TYPE_CHECK_EXACT(numObj,SlangType::Int);
	
	*res = (SlangHeader*)MakeInt(((SlangObj*)numObj)->integer+1);
	return true;
}

inline bool SlangInterpreter::SlangFuncDec(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* numObj;
	if (!WrappedEvalExpr(argIt->left,&numObj))
		return false;
	
	TYPE_CHECK_EXACT(numObj,SlangType::Int);
	
	*res = (SlangHeader*)MakeInt(((SlangObj*)numObj)->integer-1);
	return true;
}

inline bool SlangInterpreter::AddReals(SlangHeader** res,double curr){
	SlangList* argIt = GetCurrArg();
	while (argIt){
		SlangHeader* valObj = nullptr;
		if (!WrappedEvalExpr(argIt->left,&valObj))
			return false;
		
		if (GetType(valObj)==SlangType::Real){
			curr += ((SlangObj*)valObj)->real;
		} else if (GetType(valObj)==SlangType::Int){
			curr += ((SlangObj*)valObj)->integer;
		} else {
			TypeError(GetCurrArg()->left,GetType(valObj),SlangType::Real);
			return false;
		}
		
		NEXT_ARG();
		argIt = GetCurrArg();
	}
	
	*res = (SlangHeader*)MakeReal(curr);
	return true;
}

inline bool SlangInterpreter::SlangFuncAdd(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	int64_t sum = 0;
	size_t argCount = 0;
	while (argIt){
		SlangHeader* valObj = nullptr;
		if (!WrappedEvalExpr(argIt->left,&valObj))
			return false;
		
		if (GetType(valObj)==SlangType::Int){
			sum += ((SlangObj*)valObj)->integer;
		} else if (GetType(valObj)==SlangType::Real){
			NEXT_ARG();
			return AddReals(res,((SlangObj*)valObj)->real+(double)sum);
		} else {
			TypeError(GetCurrArg()->left,GetType(valObj),SlangType::Int,argCount+1);
			return false;
		}
		
		++argCount;
		NEXT_ARG();
		argIt = GetCurrArg();
	}
	
	*res = (SlangHeader*)MakeInt(sum);
	return true;
}

inline bool SlangInterpreter::SlangFuncSub(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
	
	if (!IsNumeric(l)){
		TypeError(GetCurrArg()->left,GetType(l),SlangType::Int);
		return false;
	}
	StackAddArg(l);
	
	NEXT_ARG();
	argIt = GetCurrArg();
	SlangHeader* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r))
		return false;
		
	if (!IsNumeric(r)){
		TypeError(GetCurrArg()->left,GetType(r),SlangType::Int);
		return false;
	}
	
	SlangObj* lobj = (SlangObj*)funcArgStack[--frameIndex];
	SlangObj* robj = (SlangObj*)r;
	if (lobj->header.type==SlangType::Int){
		if (robj->header.type==SlangType::Int){
			*res = (SlangHeader*)MakeInt(lobj->integer - robj->integer);
			return true;
		} else {
			*res = (SlangHeader*)MakeReal((double)lobj->integer - robj->real);
			return true;
		}
	} else {
		if (robj->header.type==SlangType::Int){
			*res = (SlangHeader*)MakeReal(lobj->real - (double)robj->integer);
			return true;
		} else {
			*res = (SlangHeader*)MakeReal(lobj->real - robj->real);
			return true;
		}
	}
}

inline bool SlangInterpreter::MulReals(SlangHeader** res,double curr){
	SlangList* argIt = GetCurrArg();
	while (argIt){
		SlangHeader* valObj = nullptr;
		if (!WrappedEvalExpr(argIt->left,&valObj))
			return false;
		
		if (GetType(valObj)==SlangType::Real){
			curr *= ((SlangObj*)valObj)->real;
		} else if (GetType(valObj)==SlangType::Int){
			curr *= ((SlangObj*)valObj)->integer;
		} else {
			TypeError(GetCurrArg()->left,GetType(valObj),SlangType::Real);
			return false;
		}
		
		NEXT_ARG();
		argIt = GetCurrArg();
	}
	
	*res = (SlangHeader*)MakeReal(curr);
	return true;
}

inline bool SlangInterpreter::SlangFuncMul(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	int64_t prod = 1;
	size_t argCount = 0;
	while (argIt){
		SlangHeader* valObj = nullptr;
		if (!WrappedEvalExpr(argIt->left,&valObj))
			return false;
			
		if (GetType(valObj)==SlangType::Int){
			prod *= ((SlangObj*)valObj)->integer;
		} else if (GetType(valObj)==SlangType::Real){
			NEXT_ARG();
			return MulReals(res,(double)prod*((SlangObj*)valObj)->real);
		} else {
			TypeError(GetCurrArg()->left,GetType(valObj),SlangType::Int,argCount+1);
			return false;
		}
		
		NEXT_ARG();
		argIt = GetCurrArg();
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

inline bool SlangInterpreter::SlangFuncDiv(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
		
	if (!IsNumeric(l)){
		TypeError(GetCurrArg()->left,GetType(l),SlangType::Int);
		return false;
	}
	StackAddArg(l);
	
	NEXT_ARG();
	argIt = GetCurrArg();
	
	SlangHeader* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r))
		return false;
		
	if (!IsNumeric(r)){
		TypeError(GetCurrArg()->left,GetType(r),SlangType::Int);
		return false;
	}
	
	SlangObj* lobj = (SlangObj*)funcArgStack[--frameIndex];
	SlangObj* robj = (SlangObj*)r;
	
	if (lobj->header.type==SlangType::Int){
		if (robj->header.type==SlangType::Int){
			if (robj->integer==0){
				ZeroDivisionError((SlangHeader*)GetCurrArg());
				return false;
			}
			*res = (SlangHeader*)MakeInt(floordiv(lobj->integer,robj->integer));
			return true;
		} else {
			if (robj->real==0.0){
				ZeroDivisionError((SlangHeader*)GetCurrArg());
				return false;
			}
			*res = (SlangHeader*)MakeReal((double)lobj->integer/robj->real);
			return true;
		}
	} else {
		if (robj->header.type==SlangType::Int){
			if (robj->integer==0){
				ZeroDivisionError((SlangHeader*)GetCurrArg());
				return false;
			}
			*res = (SlangHeader*)MakeReal(lobj->real/(double)robj->integer);
			return true;
		} else {
			if (robj->real==0.0){
				ZeroDivisionError((SlangHeader*)GetCurrArg());
				return false;
			}
			*res = (SlangHeader*)MakeReal(lobj->real/robj->real);
			return true;
		}
	}
}

inline bool SlangInterpreter::SlangFuncFloorDiv(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
		
	if (!IsNumeric(l)){
		TypeError(GetCurrArg()->left,GetType(l),SlangType::Int);
		return false;
	}
	StackAddArg(l);
	
	NEXT_ARG();
	argIt = GetCurrArg();
	
	SlangHeader* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r))
		return false;
		
	if (!IsNumeric(r)){
		TypeError(GetCurrArg()->left,GetType(r),SlangType::Int);
		return false;
	}
	
	SlangObj* lobj = (SlangObj*)funcArgStack[--frameIndex];
	SlangObj* robj = (SlangObj*)r;
	
	if (lobj->header.type==SlangType::Int){
		if (robj->header.type==SlangType::Int){
			if (robj->integer==0){
				ZeroDivisionError((SlangHeader*)GetCurrArg());
				return false;
			}
			*res = (SlangHeader*)MakeInt(floordiv(lobj->integer,robj->integer));
			return true;
		} else {
			if (robj->real==0.0){
				ZeroDivisionError((SlangHeader*)GetCurrArg());
				return false;
			}
			*res = (SlangHeader*)MakeInt((int64_t)((double)lobj->integer/robj->real));
			return true;
		}
	} else {
		if (robj->header.type==SlangType::Int){
			if (robj->integer==0){
				ZeroDivisionError((SlangHeader*)GetCurrArg());
				return false;
			}
			*res = (SlangHeader*)MakeInt((int64_t)(lobj->real/(double)robj->integer));
			return true;
		} else {
			if (robj->real==0.0){
				ZeroDivisionError((SlangHeader*)GetCurrArg());
				return false;
			}
			*res = (SlangHeader*)MakeInt((int64_t)(lobj->real/robj->real));
			return true;
		}
	}
}

inline bool SlangInterpreter::SlangFuncMod(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
		
	if (!IsNumeric(l)){
		TypeError(GetCurrArg()->left,GetType(l),SlangType::Int);
		return false;
	}
	StackAddArg(l);
	
	NEXT_ARG();
	argIt = GetCurrArg();
	SlangHeader* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r))
		return false;
		
	if (!IsNumeric(r)){
		TypeError(GetCurrArg()->left,GetType(r),SlangType::Int);
		return false;
	}
	
	SlangObj* lobj = (SlangObj*)funcArgStack[--frameIndex];
	SlangObj* robj = (SlangObj*)r;
	
	if (lobj->header.type==SlangType::Int){
		if (robj->header.type==SlangType::Int){
			if (robj->integer==0){
				ZeroDivisionError((SlangHeader*)GetCurrArg());
				return false;
			}
			*res = (SlangHeader*)MakeInt(floormod(lobj->integer,robj->integer));
			return true;
		} else {
			if (robj->real==0.0){
				ZeroDivisionError((SlangHeader*)GetCurrArg());
				return false;
			}
			*res = (SlangHeader*)MakeReal(fmod((double)lobj->integer,robj->real));
			return true;
		}
	} else {
		if (robj->header.type==SlangType::Int){
			if (robj->integer==0){
				ZeroDivisionError((SlangHeader*)GetCurrArg());
				return false;
			}
			*res = (SlangHeader*)MakeReal(fmod(lobj->real,(double)robj->integer));
			return true;
		} else {
			if (robj->real==0.0){
				ZeroDivisionError((SlangHeader*)GetCurrArg());
				return false;
			}
			*res = (SlangHeader*)MakeReal(fmod(lobj->real,robj->real));
			return true;
		}
	}
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

inline bool SlangInterpreter::SlangFuncPow(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
		
	if (!IsNumeric(l)){
		TypeError(GetCurrArg()->left,GetType(l),SlangType::Int);
		return false;
	}
	StackAddArg(l);
	
	NEXT_ARG();
	argIt = GetCurrArg();
	SlangHeader* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r))
		return false;
		
	if (!IsNumeric(r)){
		TypeError(GetCurrArg()->left,GetType(r),SlangType::Int);
		return false;
	}
	
	SlangObj* lobj = (SlangObj*)funcArgStack[--frameIndex];
	SlangObj* robj = (SlangObj*)r;
	
	if (lobj->header.type==SlangType::Int){
		if (robj->header.type==SlangType::Int){
			if (robj->integer>=0){
				*res = (SlangHeader*)MakeInt(intpow(lobj->integer,(uint64_t)robj->integer));
			} else {
				*res = (SlangHeader*)MakeReal(pow((double)lobj->integer,(double)robj->integer));
			}
			return true;
		} else {
			*res = (SlangHeader*)MakeReal(pow((double)lobj->integer,robj->real));
			return true;
		}
	} else {
		if (robj->header.type==SlangType::Int){
			*res = (SlangHeader*)MakeReal(pow(lobj->real,(double)robj->integer));
			return true;
		} else {
			*res = (SlangHeader*)MakeReal(pow(lobj->real,robj->real));
			return true;
		}
	}
}

inline bool SlangInterpreter::SlangFuncAbs(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
		
	if (!IsNumeric(l)){
		TypeError(GetCurrArg()->left,GetType(l),SlangType::Int);
		return false;
	}
	
	if (l->type==SlangType::Int){
		int64_t v = ((SlangObj*)l)->integer;
		*res = (SlangHeader*)MakeInt((v<0) ? -v : v);
	} else {
		double v = ((SlangObj*)l)->real;
		*res = (SlangHeader*)MakeReal(abs(v));
	}
	return true;
}

inline bool SlangInterpreter::SlangFuncFloor(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
		
	if (!IsNumeric(l)){
		TypeError(GetCurrArg()->left,GetType(l),SlangType::Int);
		return false;
	}
	
	if (l->type==SlangType::Int){
		int64_t v = ((SlangObj*)l)->integer;
		*res = (SlangHeader*)MakeInt(v);
	} else {
		double v = ((SlangObj*)l)->real;
		*res = (SlangHeader*)MakeReal(floor(v));
	}
	return true;
}

inline bool SlangInterpreter::SlangFuncCeil(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
		
	if (!IsNumeric(l)){
		TypeError(GetCurrArg()->left,GetType(l),SlangType::Int);
		return false;
	}
	
	if (l->type==SlangType::Int){
		int64_t v = ((SlangObj*)l)->integer;
		*res = (SlangHeader*)MakeInt(v);
	} else {
		double v = ((SlangObj*)l)->real;
		*res = (SlangHeader*)MakeReal(ceil(v));
	}
	return true;
}

inline bool SlangInterpreter::GetTwoIntArgs(SlangObj** lobj,SlangObj** robj){
	SlangList* argIt = GetCurrArg();
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
		
	TYPE_CHECK_EXACT(l,SlangType::Int);
	StackAddArg(l);
	
	NEXT_ARG();
	argIt = GetCurrArg();
	SlangHeader* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r))
		return false;
		
	TYPE_CHECK_EXACT(r,SlangType::Int);
	
	*lobj = (SlangObj*)funcArgStack[--frameIndex];
	*robj = (SlangObj*)r;
	return true;
}

inline bool SlangInterpreter::SlangFuncBitAnd(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	int64_t prod = -1;
	size_t argCount = 0;
	while (argIt){
		SlangHeader* valObj = nullptr;
		if (!WrappedEvalExpr(argIt->left,&valObj))
			return false;
			
		TYPE_CHECK_EXACT(valObj,SlangType::Int);
		
		prod &= ((SlangObj*)valObj)->integer;
		
		NEXT_ARG();
		argIt = GetCurrArg();
		++argCount;
	}
	
	*res = (SlangHeader*)MakeInt(prod);
	return true;
}

inline bool SlangInterpreter::SlangFuncBitOr(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	int64_t prod = 0;
	size_t argCount = 0;
	while (argIt){
		SlangHeader* valObj = nullptr;
		if (!WrappedEvalExpr(argIt->left,&valObj))
			return false;
			
		TYPE_CHECK_EXACT(valObj,SlangType::Int);
		
		prod |= ((SlangObj*)valObj)->integer;
		
		NEXT_ARG();
		argIt = GetCurrArg();
		++argCount;
	}
	
	*res = (SlangHeader*)MakeInt(prod);
	return true;
}

inline bool SlangInterpreter::SlangFuncBitXor(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	int64_t prod = 0;
	size_t argCount = 0;
	while (argIt){
		SlangHeader* valObj = nullptr;
		if (!WrappedEvalExpr(argIt->left,&valObj))
			return false;
			
		TYPE_CHECK_EXACT(valObj,SlangType::Int);
		
		prod ^= ((SlangObj*)valObj)->integer;
		
		NEXT_ARG();
		argIt = GetCurrArg();
		++argCount;
	}
	
	*res = (SlangHeader*)MakeInt(prod);
	return true;
}

inline bool SlangInterpreter::SlangFuncBitNot(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* intObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&intObj))
		return false;
		
	TYPE_CHECK_EXACT(intObj,SlangType::Int);
	SlangObj* intNum = (SlangObj*)intObj;
	
	*res = (SlangHeader*)MakeInt(~intNum->integer);
	return true;
}

inline bool SlangInterpreter::SlangFuncBitLeftShift(SlangHeader** res){
	SlangObj* lobj;
	SlangObj* robj;
	if (!GetTwoIntArgs(&lobj,&robj))
		return false;
		
	*res = (SlangHeader*)MakeInt(lobj->integer << robj->integer);
	return true;
}

inline bool SlangInterpreter::SlangFuncBitRightShift(SlangHeader** res){
	SlangObj* lobj;
	SlangObj* robj;
	if (!GetTwoIntArgs(&lobj,&robj))
		return false;
		
	*res = (SlangHeader*)MakeInt(lobj->integer >> robj->integer);
	return true;
}

// next four functions are identical except for the operator
inline bool SlangInterpreter::SlangFuncGT(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
		
	if (!IsNumeric(l)){
		TypeError(GetCurrArg()->left,GetType(l),SlangType::Int);
		return false;
	}
	StackAddArg(l);
	
	NEXT_ARG();
	argIt = GetCurrArg();
	SlangHeader* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r))
		return false;
		
	if (!IsNumeric(r)){
		TypeError(GetCurrArg()->left,GetType(r),SlangType::Int);
		return false;
	}
	
	SlangObj* lobj = (SlangObj*)funcArgStack[--frameIndex];
	SlangObj* robj = (SlangObj*)r;
	
	if (lobj->header.type==SlangType::Int){
		if (robj->header.type==SlangType::Int){
			*res = (SlangHeader*)MakeBool(lobj->integer > robj->integer);
			return true;
		} else {
			*res = (SlangHeader*)MakeBool((double)lobj->integer > robj->real);
			return true;
		}
	} else {
		if (robj->header.type==SlangType::Int){
			*res = (SlangHeader*)MakeBool(lobj->real > (double)robj->integer);
			return true;
		} else {
			*res = (SlangHeader*)MakeBool(lobj->real > robj->real);
			return true;
		}
	}
}

inline bool SlangInterpreter::SlangFuncLT(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
		
	if (!IsNumeric(l)){
		TypeError(GetCurrArg()->left,GetType(l),SlangType::Int);
		return false;
	}
	StackAddArg(l);
	
	NEXT_ARG();
	argIt = GetCurrArg();
	SlangHeader* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r))
		return false;
		
	if (!IsNumeric(r)){
		TypeError(GetCurrArg()->left,GetType(r),SlangType::Int);
		return false;
	}
	
	SlangObj* lobj = (SlangObj*)funcArgStack[--frameIndex];
	SlangObj* robj = (SlangObj*)r;
	
	if (lobj->header.type==SlangType::Int){
		if (robj->header.type==SlangType::Int){
			*res = (SlangHeader*)MakeBool(lobj->integer < robj->integer);
			return true;
		} else {
			*res = (SlangHeader*)MakeBool((double)lobj->integer < robj->real);
			return true;
		}
	} else {
		if (robj->header.type==SlangType::Int){
			*res = (SlangHeader*)MakeBool(lobj->real < (double)robj->integer);
			return true;
		} else {
			*res = (SlangHeader*)MakeBool(lobj->real < robj->real);
			return true;
		}
	}
}

inline bool SlangInterpreter::SlangFuncGTE(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
		
	if (!IsNumeric(l)){
		TypeError(GetCurrArg()->left,GetType(l),SlangType::Int);
		return false;
	}
	StackAddArg(l);
	
	NEXT_ARG();
	argIt = GetCurrArg();
	SlangHeader* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r))
		return false;
		
	if (!IsNumeric(r)){
		TypeError(GetCurrArg()->left,GetType(r),SlangType::Int);
		return false;
	}
	
	SlangObj* lobj = (SlangObj*)funcArgStack[--frameIndex];
	SlangObj* robj = (SlangObj*)r;
	
	if (lobj->header.type==SlangType::Int){
		if (robj->header.type==SlangType::Int){
			*res = (SlangHeader*)MakeBool(lobj->integer >= robj->integer);
			return true;
		} else {
			*res = (SlangHeader*)MakeBool((double)lobj->integer >= robj->real);
			return true;
		}
	} else {
		if (robj->header.type==SlangType::Int){
			*res = (SlangHeader*)MakeBool(lobj->real >= (double)robj->integer);
			return true;
		} else {
			*res = (SlangHeader*)MakeBool(lobj->real >= robj->real);
			return true;
		}
	}
}

inline bool SlangInterpreter::SlangFuncLTE(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
		
	if (!IsNumeric(l)){
		TypeError(GetCurrArg()->left,GetType(l),SlangType::Int);
		return false;
	}
	StackAddArg(l);
	
	NEXT_ARG();
	argIt = GetCurrArg();
	SlangHeader* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r))
		return false;
		
	if (!IsNumeric(r)){
		TypeError(GetCurrArg()->left,GetType(r),SlangType::Int);
		return false;
	}
	
	SlangObj* lobj = (SlangObj*)funcArgStack[--frameIndex];
	SlangObj* robj = (SlangObj*)r;
	
	if (lobj->header.type==SlangType::Int){
		if (robj->header.type==SlangType::Int){
			*res = (SlangHeader*)MakeBool(lobj->integer <= robj->integer);
			return true;
		} else {
			*res = (SlangHeader*)MakeBool((double)lobj->integer <= robj->real);
			return true;
		}
	} else {
		if (robj->header.type==SlangType::Int){
			*res = (SlangHeader*)MakeBool(lobj->real <= (double)robj->integer);
			return true;
		} else {
			*res = (SlangHeader*)MakeBool(lobj->real <= robj->real);
			return true;
		}
	}
}

inline bool SlangInterpreter::SlangFuncAssert(SlangHeader** res){
	SlangHeader* cond = nullptr;
	if (!WrappedEvalExpr(GetCurrArg()->left,&cond))
		return false;
	
	bool test;
	if (!ConvertToBool(cond,test)){
		TypeError(GetCurrArg()->left,GetType(cond),SlangType::Bool);
		return false;
	}
	
	if (!test){
		AssertError(GetCurrArg()->left);
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
		case SlangType::List:
		case SlangType::NullType:
		case SlangType::Env:
		case SlangType::Params:
		case SlangType::Lambda:
		case SlangType::Storage:
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

inline bool SlangInterpreter::SlangFuncEq(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
	StackAddArg(l);
	
	NEXT_ARG();
	argIt = GetCurrArg();
	SlangHeader* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r))
		return false;
	
	l = funcArgStack[--frameIndex];
	
	*res = (SlangHeader*)MakeBool(EqualObjs(l,r));
	return true;
}

inline bool SlangInterpreter::SlangFuncIs(SlangHeader** res){
	SlangList* argIt = GetCurrArg();
	SlangHeader* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l))
		return false;
	
	StackAddArg(l);
	
	SlangHeader* r = nullptr;
	NEXT_ARG();
	argIt = GetCurrArg();
	if (!WrappedEvalExpr(argIt->left,&r))
		return false;
	
	l = funcArgStack[--frameIndex];
	
	*res = (SlangHeader*)MakeBool(IdenticalObjs(l,r));
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

inline void SlangInterpreter::PushArg(SlangList* list){
	if (argIndex>=argStack.size()-1){
		argStack.resize(argStack.size()*3/2);
	}
	argStack[++argIndex] = list;
}

inline void SlangInterpreter::PopArg(){
	assert(argIndex!=0);
	--argIndex;
}

inline void SlangInterpreter::PushExpr(SlangHeader* expr){
	if (exprIndex>=exprStack.size()-1){
		exprStack.resize(exprStack.size()*3/2);
	}
	exprStack[++exprIndex] = expr;
	if (expr && expr->type==SlangType::List){
		PushArg((SlangList*)expr);
	} else {
		PushArg(nullptr);
	}
}

inline void SlangInterpreter::SetExpr(SlangHeader* expr){
	exprStack[exprIndex] = expr;
	if (expr && expr->type==SlangType::List){
		argStack[argIndex] = (SlangList*)expr;
	} else {
		argStack[argIndex] = nullptr;
	}
}

inline void SlangInterpreter::PopExpr(){
	assert(exprIndex!=0);
	PopArg();
	--exprIndex;
}

inline bool SlangInterpreter::WrappedEvalExpr(SlangHeader* expr,SlangHeader** res){
	++gCurrDepth;
	if (gCurrDepth>gMaxDepth) gMaxDepth = gCurrDepth;
	size_t frameStart = frameIndex;
	PushEnv(GetCurrEnv());
	PushExpr(expr);
	bool b = EvalExpr(res);
	PopExpr();
	PopEnv();
	--gCurrDepth;
	frameIndex = frameStart;
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
			//std::vector<SlangHeader*>& exprs,
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
		StackAddArg(pair->left);
		
		if (pair->right){
			// too many args in init pair
			ArityError(paramIt->left,3,2,2);
			return false;
		}
		
		if (!IsList(paramIt->right)){
			EvalError((SlangHeader*)paramIt);
			return false;
		}
		paramIt = (SlangList*)paramIt->right;
	}
	return true;
}

#define ARITY_EXACT_CHECK(x) \
	if (argCount!=(x)){ \
		ArityError(((SlangList*)GetCurrExpr())->left,argCount,(x),(x)); \
		return false; \
	}
	
#define ARITY_BETWEEN_CHECK(x,y) \
	if (argCount<(x)||argCount>(y)){ \
		ArityError(((SlangList*)GetCurrExpr())->left,argCount,(x),(y)); \
		return false; \
	}
	
#define ARITY_AT_LEAST_CHECK(x) \
	if (argCount<(x)){ \
		ArityError(((SlangList*)GetCurrExpr())->left,argCount,(x),VARIADIC_ARG_COUNT); \
		return false; \
	}
	
#define ARITY_AT_MOST_CHECK(x) \
	if (argCount>(x)){ \
		ArityError(((SlangList*)GetCurrExpr())->left,argCount,(x),VARIADIC_ARG_COUNT); \
		return false; \
	}

bool SlangInterpreter::EvalExpr(SlangHeader** res){
	++gEvalRecurCounter;
	SlangHeader* expr;
	while (true){
		expr = GetCurrExpr();
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
			case SlangType::Vector:
			case SlangType::Env:
			case SlangType::Storage:
			case SlangType::Params:
			case SlangType::Maybe:
			case SlangType::InputStream:
			case SlangType::OutputStream:
			case SlangType::EndOfFile:
			case SlangType::Lambda: {
				*res = expr;
				return true;
			}
			case SlangType::String: {
				if (arena->InCurrSet((uint8_t*)expr)){
					*res = expr;
					return true;
				}
				*res = Copy(expr);
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
				
				UndefinedError(expr);
				return false;
			}
			
			// function
			case SlangType::List: {
				SlangHeader* head = nullptr;
				SlangList* argIt = GetCurrArg();
				// optimization for predefined syms
				if (GetType(argIt->left)==SlangType::Symbol&&
					((SlangObj*)argIt->left)->symbol<GLOBAL_SYMBOL_COUNT){
					if (!GetRecursiveSymbol(GetCurrEnv(),((SlangObj*)argIt->left)->symbol,&head))
						head = argIt->left;
				} else {
					if (!WrappedEvalExpr(argIt->left,&head)){
						EvalError(expr);
						return false;
					}
				}
				NEXT_ARG();
				argIt = GetCurrArg();
				size_t base = frameIndex;
				
				size_t argCount = GetArgCount(argIt);
				
				SlangType headType = GetType(head);
				if (headType==SlangType::Lambda){
					SlangLambda* lam = (SlangLambda*)head;
					size_t headIndex = frameIndex;
					StackAddArg(head);
					base = frameIndex;
					SlangParams* funcParams = lam->params;
					bool variadic = lam->header.flags&FLAG_VARIADIC;
					size_t paramCount = (funcParams) ? funcParams->size : 0;
					if (variadic){
						ARITY_AT_LEAST_CHECK(paramCount-1);
					} else {
						ARITY_EXACT_CHECK(paramCount);
					}
					
					size_t orderedArgCount = (variadic) ? paramCount-1 : paramCount;
					for (size_t i=0;i<orderedArgCount;++i){
						SlangHeader* fArg = nullptr;
						if (!WrappedEvalExpr(argIt->left,&fArg)){
							EvalError(expr);
							return false;
						}
						StackAddArg(fArg);
						NEXT_ARG();
						argIt = GetCurrArg();
					}
					
					lam = (SlangLambda*)funcArgStack[headIndex];
					for (size_t i=0;i<orderedArgCount;++i){
						lam->env->SetSymbol(lam->params->params[i],funcArgStack[base+i]);
					}
					
					if (variadic){
						size_t variadicBase = frameIndex;
						while (argIt){
							SlangHeader* fArg = nullptr;
							if (!WrappedEvalExpr(argIt->left,&fArg)){
								EvalError(expr);
								return false;
							}
							StackAddArg(fArg);
							NEXT_ARG();
							argIt = GetCurrArg();
						}
						
						SlangList* list = MakeList(funcArgStack,variadicBase,frameIndex);
						lam = (SlangLambda*)funcArgStack[headIndex];
						lam->env->SetSymbol(
							lam->params->params[paramCount-1],
							(SlangHeader*)list
						);
					}
					
					lam = (SlangLambda*)funcArgStack[headIndex];
					SetExpr(lam->expr);
					
					envStack[envIndex] = lam->env;
					frameIndex = base;
					--frameIndex;
					continue;
				} else if (headType==SlangType::Symbol){
					SymbolName symbol = ((SlangObj*)head)->symbol;
					frameIndex = base;
					// handle predefined symbols
					bool success = false;
					switch (symbol){
						case SLANG_DEFINE:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncDefine(res);
							return success;
						case SLANG_LAMBDA:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncLambda(res);
							return success;
						case SLANG_LET: {
							ARITY_BETWEEN_CHECK(2,3);
							
							SymbolName letName;
							// named let
							if (GetType(argIt->left)==SlangType::Symbol){
								letName = ((SlangObj*)argIt->left)->symbol;
								NEXT_ARG();
								argIt = GetCurrArg();
							} else {
								letName = LET_SELF_SYM;
							}
							
							std::vector<SymbolName> params{};
							params.reserve(4);
							SlangHeader* paramIt = argIt->left;
							if (paramIt&&GetType(paramIt)!=SlangType::List){
								TypeError(paramIt,GetType(paramIt),SlangType::List);
								return false;
							}
							size_t baseInitExpr = frameIndex;
							if (!GetLetParams(params,(SlangList*)paramIt)){
								EvalError(paramIt);
								return false;
							}
							NEXT_ARG();
							argIt = GetCurrArg();
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
								if (!WrappedEvalExpr(funcArgStack[baseInitExpr+i],&initObj)){
									EvalError(funcArgStack[baseInitExpr+i]);
									return false;
								}
								if (!GetCurrEnv()->SetSymbol(param,initObj)){
									EvalError(funcArgStack[baseInitExpr+i]);
									return false;
								}
								++i;
							}
							
							tempLambda = (SlangLambda*)funcArgStack[--frameIndex];
							frameIndex = baseInitExpr;
							// last call tail call
							SetExpr(tempLambda->expr);
							continue;
						}
						case SLANG_SET:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncSet(res);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_DO: {
							ARITY_AT_LEAST_CHECK(1);
							// while next arg is not null
							while (argIt->right){
								if (!WrappedEvalExpr(argIt->left,res)){
									EvalError(GetCurrArg()->left);
									return false;
								}
								NEXT_ARG();
								argIt = GetCurrArg();
							}
							
							// elide last call
							SetExpr(argIt->left);
							continue;
						}
						case SLANG_PARSE: {
							ARITY_EXACT_CHECK(1);
							
							SlangHeader* strObj;
							if (!WrappedEvalExpr(argIt->left,&strObj))
								return false;
								
							if (GetType(strObj)!=SlangType::String){
								TypeError(strObj,GetType(strObj),SlangType::String);
								return false;
							}
							
							SlangStr* str = (SlangStr*)strObj;
							*res = ParseSlangString(*str);
							
							return true;
						}
						case SLANG_EVAL: {
							ARITY_EXACT_CHECK(1);
							
							SlangHeader* newExpr;
							if (!WrappedEvalExpr(argIt->left,&newExpr))
								return false;
							
							SetExpr(newExpr);
							continue;
						}
						case SLANG_APPLY: {
							// (apply func args)
							// (apply func arg1 arg2 ... argN args)
							ARITY_AT_LEAST_CHECK(2);
							SlangHeader* head = argIt->left;
							StackAddArg(head);
							NEXT_ARG();
							argIt = GetCurrArg();
							size_t preArgsBase = frameIndex;
							
							while (argIt->right){
								SlangHeader* a;
								if (!WrappedEvalExpr(argIt->left,&a))
									return false;
									
								StackAddArg(a);
								
								NEXT_ARG();
								argIt = GetCurrArg();
								if (!IsList((SlangHeader*)argIt)){
									TypeError((SlangHeader*)argIt,
												GetType((SlangHeader*)argIt),
												SlangType::List);
									return false;
								}
							}
							
							size_t preArgsEnd = frameIndex;
							
							SlangHeader* args;
							if (!WrappedEvalExpr(argIt->left,&args))
								return false;
								
							if (!IsList(args)){
								TypeError(args,GetType(args),SlangType::List);
								return false;
							}
							
							StackAddArg(args);
							
							SlangList* listHead = AllocateList();
							if (preArgsBase!=preArgsEnd){
								SlangList* preArgsList = MakeList(funcArgStack,preArgsBase,preArgsEnd);
								ConcatLists(preArgsList,(SlangList*)funcArgStack[--frameIndex]);
								listHead->right = (SlangHeader*)preArgsList;
							} else {
								listHead->right = funcArgStack[--frameIndex]; // args
							}
							frameIndex = preArgsBase;
							
							listHead->left = funcArgStack[--frameIndex]; // head
							
							SetExpr((SlangHeader*)listHead);
							continue;
						}
						case SLANG_IF: {
							ARITY_BETWEEN_CHECK(2,3);
							bool test;
							SlangHeader* condObj;
							if (!WrappedEvalExpr(argIt->left,&condObj)){
								EvalError(GetCurrArg()->left);
								return false;
							}
							if (!ConvertToBool(condObj,test)){
								TypeError(GetCurrArg()->left,GetType(GetCurrArg()->left),SlangType::Bool);
								EvalError(expr);
								return false;
							}
							
							NEXT_ARG();
							argIt = GetCurrArg();
							if (test){
								SetExpr(argIt->left);
								continue;
							}
							NEXT_ARG();
							argIt = GetCurrArg();
							if (!argIt){
								*res = nullptr;
								return true;
							}
							SetExpr(argIt->left);
							continue;
						}
						case SLANG_COND: {
							SlangHeader* val = nullptr;
							SlangHeader* elide = nullptr;
							bool test;
							while (argIt){
								TYPE_CHECK_EXACT(argIt,SlangType::List);
								SlangList* clauseIt = (SlangList*)argIt->left;
								StackAddArg((SlangHeader*)clauseIt);
								TYPE_CHECK_EXACT(clauseIt,SlangType::List);
								if (!WrappedEvalExpr(clauseIt->left,&val)){
									EvalError(((SlangList*)funcArgStack[frameIndex-1])->left);
									return false;
								}
								
								if (!ConvertToBool(val,test)){
									SlangHeader* t = ((SlangList*)funcArgStack[frameIndex-1])->left;
									TypeError(val,
											GetType(val),
											SlangType::Bool);
									EvalError(t);
									return false;
								}
								
								if (test){
									clauseIt = (SlangList*)funcArgStack[--frameIndex];
									clauseIt = (SlangList*)clauseIt->right;
									if (!clauseIt){
										*res = val;
										return true;
									}
									SlangHeader* r;
									while (clauseIt->right){
										TYPE_CHECK_EXACT(clauseIt,SlangType::List);
										StackAddArg((SlangHeader*)clauseIt);
										
										if (!WrappedEvalExpr(clauseIt->left,&r)){
											EvalError(((SlangList*)funcArgStack[--frameIndex])->left);
											return false;
										}
										clauseIt = (SlangList*)funcArgStack[--frameIndex];
										clauseIt = (SlangList*)clauseIt->right;
									}
									TYPE_CHECK_EXACT(clauseIt,SlangType::List);
									elide = clauseIt->left;
									break;
								}
								
								NEXT_ARG();
								argIt = GetCurrArg();
							}
							if (elide){
								SetExpr(elide);
								continue;
							}
							*res = nullptr;
							return true;
						}
						case SLANG_CASE: {
							ARITY_AT_LEAST_CHECK(1);
							SlangHeader* testObj;
							SlangHeader* elide = nullptr;
							bool done = false;
							if (!WrappedEvalExpr(argIt->left,&testObj)){
								EvalError(GetCurrArg()->left);
								return false;
							}
							
							NEXT_ARG();
							argIt = GetCurrArg();
							while (argIt){
								SlangList* clauseIt = (SlangList*)argIt->left;
								TYPE_CHECK_EXACT(clauseIt,SlangType::List);
								SlangList* objIt = (SlangList*)clauseIt->left;
								bool elseSkip = false;
								if (GetType((SlangHeader*)objIt)==SlangType::Symbol){
									if (((SlangObj*)objIt)->symbol==SLANG_ELSE){
										elseSkip = true;
									}
								}
								while (objIt){
									if (!elseSkip){
										TYPE_CHECK_EXACT(objIt,SlangType::List);
									}
									if (elseSkip||EqualObjs(objIt->left,testObj)){
										// inner loop
										clauseIt = (SlangList*)clauseIt->right;
										if (!clauseIt){
											*res = testObj;
											return true;
										}
										TYPE_CHECK_EXACT(clauseIt,SlangType::List);
										SlangHeader* r;
										while (clauseIt->right){
											TYPE_CHECK_EXACT(clauseIt,SlangType::List);
											StackAddArg((SlangHeader*)clauseIt);
											
											if (!WrappedEvalExpr(clauseIt->left,&r)){
												EvalError(
													((SlangList*)funcArgStack[--frameIndex])->left
												);
												return false;
											}
											clauseIt = (SlangList*)funcArgStack[--frameIndex];
											clauseIt = (SlangList*)clauseIt->right;
										}
										TYPE_CHECK_EXACT(clauseIt,SlangType::List);
										elide = clauseIt->left;
										done = true;
										break;
									}
									objIt = (SlangList*)objIt->right;
								}
								
								if (done)
									break;
								
								NEXT_ARG();
								argIt = GetCurrArg();
							}
							if (elide){
								SetExpr(elide);
								continue;
							}
							
							*res = nullptr;
							return true;
						}
						case SLANG_AND: {
							bool test;
							SlangHeader* condObj = nullptr;
							while (argIt){
								if (!WrappedEvalExpr(argIt->left,&condObj)){
									EvalError(GetCurrArg()->left);
									return false;
								}
									
								if (!ConvertToBool(condObj,test)){
									TypeError(GetCurrArg()->left,
											GetType(GetCurrArg()->left),
											SlangType::Bool);
									EvalError(expr);
									return false;
								}
								
								if (!test){
									*res = condObj;
									return true;
								}
								
								NEXT_ARG();
								argIt = GetCurrArg();
							}
							if (!condObj)
								*res = MakeBool(true);
							else
								*res = condObj;
							return true;
						}
						case SLANG_OR: {
							bool test;
							SlangHeader* condObj = nullptr;
							while (argIt){
								if (!WrappedEvalExpr(argIt->left,&condObj)){
									EvalError(GetCurrArg()->left);
									return false;
								}
									
								if (!ConvertToBool(condObj,test)){
									TypeError(GetCurrArg()->left,
											GetType(GetCurrArg()->left),
											SlangType::Bool);
									EvalError(expr);
									return false;
								}
								
								if (test){
									*res = condObj;
									return true;
								}
								
								NEXT_ARG();
								argIt = GetCurrArg();
							}
							if (!condObj)
								*res = MakeBool(false);
							else
								*res = condObj;
							return true;
						}
						case SLANG_LEN:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncLen(res);
							return success;
						case SLANG_QUOTE:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncQuote(res);
							return success;
						case SLANG_NOT:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncNot(res);
							return success;
						case SLANG_NEG:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncNegate(res);
							return success;
						case SLANG_TRY: {
							if (argCount==0){
								*res = (SlangHeader*)MakeMaybe(nullptr,false);
								return true;
							}
							
							SlangHeader* maybeRes = nullptr;
							while (argIt){
								if (!WrappedEvalExpr(argIt->left,&maybeRes)){
									*res = (SlangHeader*)MakeMaybe(nullptr,false);
									errors.clear();
									return true;
								}
								NEXT_ARG();
								argIt = GetCurrArg();
							}
							*res = (SlangHeader*)MakeMaybe(maybeRes,true);
							return true;
						}
						case SLANG_UNWRAP:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncUnwrap(res);
							return success;
						case SLANG_EMPTY:
							ARITY_EXACT_CHECK(1);
							if (!WrappedEvalExpr(argIt->left,res))
								return false;
							
							if (GetType(*res)!=SlangType::Maybe){
								TypeError(GetCurrArg()->left,GetType(GetCurrArg()->left),SlangType::Maybe);
								return false;
							}
							
							*res = (SlangHeader*)MakeBool(!((*res)->flags & FLAG_MAYBE_OCCUPIED));
							return true;
						case SLANG_OUTPUT_TO:
							ARITY_AT_LEAST_CHECK(1);
							success = SlangFuncOutputTo(res);
							return success;
						case SLANG_INPUT_FROM:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncInputFrom(res);
							return success;
						case SLANG_LIST:
							success = SlangFuncList(res);
							return success;
						case SLANG_VEC:
							success = SlangFuncVec(res);
							return success;
						case SLANG_VEC_ALLOC:
							ARITY_BETWEEN_CHECK(1,2);
							success = SlangFuncVecAlloc(res);
							return success;
						case SLANG_VEC_GET:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncVecGet(res);
							return success;
						case SLANG_VEC_SET:
							ARITY_EXACT_CHECK(3);
							success = SlangFuncVecSet(res);
							return success;
						case SLANG_VEC_APPEND:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncVecAppend(res);
							return success;
						case SLANG_VEC_POP:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncVecPop(res);
							return success;
						case SLANG_STR_GET:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncStrGet(res);
							return success;
						case SLANG_STR_SET:
							ARITY_EXACT_CHECK(3);
							success = SlangFuncStrSet(res);
							return success;
						case SLANG_MAKE_STR_ISTREAM:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncMakeStrIStream(res);
							return success;
						case SLANG_MAKE_STR_OSTREAM:
							ARITY_AT_MOST_CHECK(1);
							success = SlangFuncMakeStrOStream(res);
							return success;
						case SLANG_STREAM_GET_STR:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncStreamGetStr(res);
							return success;
						case SLANG_STREAM_WRITE:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncStreamWrite(res);
							return success;
						case SLANG_STREAM_READ:
							ARITY_BETWEEN_CHECK(1,2);
							success = SlangFuncStreamRead(res);
							return success;
						case SLANG_STREAM_WRITE_BYTE:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncStreamWriteByte(res);
							return success;
						case SLANG_STREAM_READ_BYTE:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncStreamReadByte(res);
							return success;
						case SLANG_STREAM_SEEK_BEGIN:
							ARITY_BETWEEN_CHECK(1,2);
							success = SlangFuncStreamSeekBegin(res);
							return success;
						case SLANG_STREAM_SEEK_END:
							ARITY_BETWEEN_CHECK(1,2);
							success = SlangFuncStreamSeekEnd(res);
							return success;
						case SLANG_STREAM_SEEK_OFFSET:
							ARITY_BETWEEN_CHECK(1,2);
							success = SlangFuncStreamSeekOffset(res);
							return success;
						case SLANG_STREAM_TELL:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncStreamTell(res);
							return success;
						case SLANG_PAIR:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncPair(res);
							return success;
						case SLANG_LEFT:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncLeft(res);
							return success;
						case SLANG_RIGHT:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncRight(res);
							return success;
						case SLANG_INC:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncInc(res);
							return success;
						case SLANG_DEC:	
							ARITY_EXACT_CHECK(1);
							success = SlangFuncDec(res);
							return success;
						case SLANG_ADD:
							success = SlangFuncAdd(res);
							return success;
						case SLANG_SUB:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncSub(res);
							return success;
						case SLANG_MUL:
							success = SlangFuncMul(res);
							return success;
						case SLANG_DIV:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncDiv(res);
							return success;
						case SLANG_FLOOR_DIV:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncFloorDiv(res);
							return success;
						case SLANG_MOD:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncMod(res);
							return success;
						case SLANG_POW:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncPow(res);
							return success;
						case SLANG_ABS:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncAbs(res);
							return success;
						case SLANG_FLOOR:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncFloor(res);
							return success;
						case SLANG_CEIL:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncCeil(res);
							return success;
						case SLANG_BITAND:
							success = SlangFuncBitAnd(res);
							return success;
						case SLANG_BITOR:
							success = SlangFuncBitOr(res);
							return success;
						case SLANG_BITXOR:
							success = SlangFuncBitXor(res);
							return success;
						case SLANG_BITNOT:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncBitNot(res);
							return success;
						case SLANG_LEFTSHIFT:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncBitLeftShift(res);
							return success;
						case SLANG_RIGHTSHIFT:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncBitRightShift(res);
							return success;
						case SLANG_GT:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncGT(res);
							return success;
						case SLANG_LT:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncLT(res);
							return success;
						case SLANG_GTE:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncGTE(res);
							return success;
						case SLANG_LTE:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncLTE(res);
							return success;
						case SLANG_SET_LEFT:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncSetLeft(res);
							return success;
						case SLANG_SET_RIGHT:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncSetRight(res);
							return success;
						case SLANG_ASSERT:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncAssert(res);
							return success;
						case SLANG_EQ:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncEq(res);
							return success;
						case SLANG_IS:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncIs(res);
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
						case SLANG_IS_MAYBE:
							ARITY_EXACT_CHECK(1);
							if (!WrappedEvalExpr(argIt->left,res))
								return false;
							
							*res = (SlangHeader*)MakeBool(GetType(*res)==SlangType::Maybe);
							return true;
						case SLANG_IS_EOF:
							ARITY_EXACT_CHECK(1);
							if (!WrappedEvalExpr(argIt->left,res))
								return false;
							
							*res = (SlangHeader*)MakeBool(GetType(*res)==SlangType::EndOfFile);
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
							const ExternalFunc& extF = extFuncs.at(symbol);
							if (extF.minArgs>argCount||
								(extF.maxArgs!=VARIADIC_ARG_COUNT&&extF.maxArgs<argCount)){
								ArityError(head,argCount,extF.minArgs,extF.maxArgs);
								return false;
							}
							success = extF.func(this,res);
							if (!success)
								EvalError(expr);
							return success;
					}
				} else {
					//PrintCurrEnv(GetCurrEnv());
					TypeError(head,headType,SlangType::Lambda);
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
	std::stringstream msg = {};
	
	msg << "EvalError:\n    Error while evaluating '";
	if (expr)
		msg << *expr << "'";
	else
		msg << "()'";
	PushError(expr,msg.str());
}

void SlangInterpreter::ValueError(const SlangHeader* expr,const std::string& m){
	std::stringstream msg = {};
	msg << "ValueError:\n    " << m;
	PushError(expr,msg.str());
}

void SlangInterpreter::StreamError(const SlangHeader* expr,const std::string& m){
	std::stringstream msg = {};
	msg << "StreamError:\n    " << m;
	PushError(expr,msg.str());
}

void SlangInterpreter::FileError(const SlangHeader* expr,const std::string& m){
	std::stringstream msg = {};
	msg << "FileError:\n    " << m;
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

void SlangInterpreter::UnwrapError(const SlangHeader* expr){
	std::stringstream msg = {};
	msg << "UnwrapError:\n    Could not unwrap empty Maybe in argument '";
	if (!expr)
		msg << "()'";
	else
		msg << *expr << "'";
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

void SlangInterpreter::ArityError(const SlangHeader* head,size_t found,size_t expectMin,size_t expectMax){
	std::stringstream msg = {};
	const char* plural = (expectMin!=1) ? "s" : "";
	
	msg << "ArityError:\n    Procedure '";
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
		
	PushError(head,msg.str());
}

void SlangInterpreter::ZeroDivisionError(const SlangHeader* head){
	std::stringstream msg = {};
	
	msg << "ZeroDivisionError:\n    Division by zero";
		
	PushError(head,msg.str());
}

void PrintInfo(){
	std::cout << "Alloc balance: " << gAllocCount << '\n';
	std::cout << "Alloc total: " << gAllocTotal << " (" << gAllocTotal/1024 << " KB)\n";
	//std::cout << "Tenure count: " << gTenureCount << " (" << gTenureCount*sizeof(SlangObj)/1024 << " KB)\n";
	//std::cout << "Tenure set count: " << tenureCount << '\n';
	size_t codeSize = gDebugParser->codePointer-gDebugParser->codeStart;
	std::cout << "Code alloc total: " << codeSize << " (" << codeSize/1024 << " KB)\n";
	std::cout << "Eval count: " << gEvalCounter << '\n';
	std::cout << "Non-eliminated eval count: " << gEvalRecurCounter << '\n';
	std::cout << "Max stack height: " << gMaxStackHeight << '\n';
	std::cout << "Small GCs: " << gSmallGCs << '\n';
	std::cout << "Realloc count: " << gReallocCount << '\n';
	std::cout << "Max arena size: " << gMaxArenaSize/1024 << " KB\n";
	std::cout << "Curr arena size: " << gArenaSize/1024 << " KB\n";
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
}

void SlangInterpreter::SetGlobalSymbol(const std::string& name,SlangHeader* value){
	SymbolName sym = parser.RegisterSymbol(name);
	SlangEnv* globalEnv = envStack.front();
	if (!globalEnv->SetSymbol(sym,value)){
		DefEnvSymbol(globalEnv,sym,value);
	}
}

bool SlangInterpreter::GetGlobalSymbol(const std::string& name,SlangHeader** value){
	SymbolName sym = parser.RegisterSymbol(name);
	SlangEnv* globalEnv = envStack.front();
	return globalEnv->GetSymbol(sym,value);
}

bool SlangInterpreter::CallLambda(
		const SlangLambda* lam,
		const std::vector<SlangHeader*>& args,
		SlangHeader** res){
	bool variadic = (lam->header.flags & FLAG_VARIADIC);
	if (!variadic&&args.size()!=lam->params->size){
		ArityError((SlangHeader*)lam,args.size(),lam->params->size,lam->params->size);
		return false;
	}
	
	size_t i=0;
	for (auto* arg : args){
		lam->env->SetSymbol(lam->params->params[i],arg);
		++i;
	}
	
	return WrappedEvalExpr(lam->expr,res);
}

bool SlangInterpreter::Run(SlangHeader* prog,SlangHeader** res){
	errors.clear();
	auto start = std::chrono::steady_clock::now();
	WrappedEvalExpr(prog,res);
	std::chrono::duration<double> diff = std::chrono::steady_clock::now()-start;
	gRunTime = diff.count();
	
	if (!errors.empty()){
		PrintErrors(errors);
		//PrintCurrEnv(GetCurrEnv());
	} else {
		assert(frameIndex==0);
		assert(envIndex==0);
		assert(exprIndex==0);
	}
	return errors.empty();
}

inline void SlangInterpreter::AddExternalFunction(const ExternalFunc& ext){
	SymbolName sym = parser.RegisterSymbol(ext.name);
	extFuncs[sym] = ext;
}

inline void SlangInterpreter::AddFinalizer(const Finalizer& f){
	assert(f.obj);
	finalizers.push_back(f);
}

inline void SlangInterpreter::RemoveFinalizer(SlangHeader* obj){
	for (ssize_t i=finalizers.size()-1;i>=0;--i){
		if (obj==finalizers[i].obj){
			finalizers.erase(finalizers.begin()+i);
			break;
		}
	}
}

SlangInterpreter::SlangInterpreter(){
	gRandState.Seed(GetSeedTime());
	
	extFuncs = {};
	AddExternalFunction({"gc-collect",&GCManualCollect});
	AddExternalFunction({"gc-rec-size",&GCRecObjSize,1,1});
	AddExternalFunction({"gc-size",&GCObjSize,1,1});
	AddExternalFunction({"gc-mem-size",GCMemSize});
	AddExternalFunction({"gc-mem-cap",&GCMemCapacity});
	
	AddExternalFunction({"input",&ExtFuncInput});
	AddExternalFunction({"output",&ExtFuncOutput,0,VARIADIC_ARG_COUNT});
	AddExternalFunction({"print",&ExtFuncPrint,0,VARIADIC_ARG_COUNT});
	AddExternalFunction({"make-file-istream",&ExtFuncFileOpenRead,1,1});
	AddExternalFunction({"make-file-ostream",&ExtFuncFileOpenWrite,1,1});
	AddExternalFunction({"make-file-ostream-app",&ExtFuncFileOpenWriteApp,1,1});
	AddExternalFunction({"file-close!",&ExtFuncFileClose,1,1});
	AddExternalFunction({"flush!",&ExtFuncFileFlush,1,1});
	AddExternalFunction({"stdin",&ExtFuncStdIn});
	AddExternalFunction({"stdout",&ExtFuncStdOut});
	AddExternalFunction({"stderr",&ExtFuncStdErr});
	AddExternalFunction({"file?",&ExtFuncIsFile,1,1});
	AddExternalFunction({"file-open?",&ExtFuncIsFileOpen,1,1});
	
	AddExternalFunction({"time!",&ExtFuncGetTime});
	AddExternalFunction({"perftime!",&ExtFuncGetPerfTime});
	AddExternalFunction({"rand!",&ExtFuncRand});
	AddExternalFunction({"rand-seed!",&ExtFuncRandSeed,1,1});
	
	arena = new MemArena();
	size_t memSize = SMALL_SET_SIZE*2;
	uint8_t* memAlloc = (uint8_t*)malloc(memSize);
	arena->SetSpace(memAlloc,memSize,memAlloc);
	memset(arena->memSet,0,memSize);
	gArenaSize = memSize;
	gMaxArenaSize = memSize;
	gDebugInterpreter = this;
	
	funcArgStack.resize(32);
	envStack.resize(16);
	exprStack.resize(32);
	argStack.resize(32);
	finalizers.clear();
	finalizers.reserve(32);
	frameIndex = 0;
	envIndex = 0;
	exprIndex = 0;
	argIndex = 0;
	errors = {};
	genIndex = 1;
	SlangEnv* env = AllocateEnv(4);
	env->parent = nullptr;
	envStack[0] = env;
	argStack[0] = nullptr;
	exprStack[0] = nullptr;
}

SlangInterpreter::~SlangInterpreter(){
	for (auto& finalizer : finalizers){
		finalizer.func(this,finalizer.obj);
	}
	finalizers.clear();
	frameIndex = 0;
	envIndex = 0;
	exprIndex = 0;
	argIndex = 0;
	SmallGC(0);
	
	if (arena->memSet)
		free(arena->memSet);
	delete arena;
}

SlangHeader* SlangInterpreter::Parse(const std::string& code){
	SlangHeader* parsed = parser.ParseString(code);
	if (!parser.errors.empty()){
		PrintErrors(parser.errors);
		return nullptr;
	}
	return parsed;
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
	} else if (c==';'&&nextC=='-'){
		token.type = SlangTokenType::Comment;
		char lastC = ' ';
		++pos;
		++pos;
		while (pos!=end){
			if (*pos==';'&&lastC=='-'){
				++pos;
				++pos;
				break;
			}
			if (*pos=='\n') ++line;
			lastC = *pos;
			++pos;
		}
	} else if (c==';'){
		token.type = SlangTokenType::Comment;
		while (pos!=end&&*pos!='\n') ++pos;
	} else if ((c>='0'&&c<='9')||(c=='.'&&nextC>='0'&&nextC<='9')){
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
			std::stringstream msg{};
			msg << "Expected closing bracket, not '" << *pos << "'";
			parser->PushError(msg.str());
			token.type = SlangTokenType::Error;
			++pos;
		}
	} else if (c=='.'&&IsWhitespace(nextC)){
		token.type = SlangTokenType::Dot;
		++pos;
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
	if (nameDict.contains(symbol))
		return nameDict.at(symbol);
		
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
	std::stringstream ss{};
	ss << "SyntaxError:\n    " << msg;
	LocationData l = {token.line,token.col,0};
	errors.emplace_back(l,ss.str());
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

inline SlangHeader* SlangParser::CodeAllocHeader(){
	SlangHeader* obj = (SlangHeader*)CodeAlloc(sizeof(SlangHeader));
	obj->flags = 0;
	return obj;
}

inline SlangStr* SlangParser::CodeAllocStr(size_t size){
	size_t qsize = QuantizeSize(size);
	SlangStorage* storage = (SlangStorage*)CodeAlloc(sizeof(SlangStorage)+sizeof(uint8_t)*qsize);
	SlangStr* obj = (SlangStr*)CodeAlloc(sizeof(SlangStr));
	obj->header.type = SlangType::String;
	obj->header.flags = 0;
	obj->storage = storage;
	storage->header.type = SlangType::Storage;
	storage->header.flags = 0;
	storage->header.elemSize = sizeof(uint8_t);
	storage->size = size;
	storage->capacity = qsize;
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
			char* end;
			errno = 0;
			double r = strtod(token.view.begin(),&end);
			NextToken();
			if (errno==ERANGE){
				PushError("Could not parse real: "+std::string(token.view));
				return false;
			}
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
			s = RegisterSymbol(copy);
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
			SlangHeader* boolObj = CodeAllocHeader();
			boolObj->type = SlangType::Bool;
			boolObj->boolVal = true;
			codeMap[boolObj] = loc;
			*res = boolObj;
			return true;
		}
		case SlangTokenType::False: {
			NextToken();
			SlangHeader* boolObj = CodeAllocHeader();
			boolObj->type = SlangType::Bool;
			boolObj->boolVal = false;
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
		case SLANG_DEFINE: {
			// (def (func arg1 arg2...) expr1 expr2...) ->
			// (def func (& (arg1 arg2...) expr1 expr2...))
			if (argCount<3) return;
			SlangList* next = (SlangList*)list->right;
			if (GetType(next->left)!=SlangType::List)
				return;
			
			SlangList* args = (SlangList*)next->left;
			SlangHeader* funcName = args->left;
			SlangList* exprsStart = (SlangList*)next->right;
			
			SlangList* lambdaWrapper = WrapExprSplice(SLANG_LAMBDA,exprsStart);
			SlangList* inbetween = CodeAllocList();
			inbetween->right = lambdaWrapper->right;
			lambdaWrapper->right = (SlangHeader*)inbetween;
			inbetween->left = args->right;
			next->left = funcName;
			SlangList* lambdaList = CodeAllocList();
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
			PushError("Expected ')'! (From @"+std::to_string(line+1)+","+std::to_string(col+1)+")");
			return false;
		}
		
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

SlangHeader* SlangParser::Parse(){
	gDebugParser = this;
	errors.clear();
	
	token.type = SlangTokenType::Comment;
	NextToken();
	
	auto start = std::chrono::steady_clock::now();
	
	std::vector<SlangHeader*> exprs = {};
	SlangHeader* temp;
	while (errors.empty()){
		if (!ParseObj(&temp)) break;
		exprs.push_back(temp);
		
		if (token.type==SlangTokenType::EndOfFile) break;
	}
	if (!errors.empty()) return nullptr;
	
	SlangHeader* prog = nullptr;
	if (exprs.size()==1){
		prog = exprs.front();
	} else if (exprs.size()>1){
		prog = WrapProgram(exprs);
	}
	
	std::chrono::duration<double> diff = std::chrono::steady_clock::now()-start;
	gParseTime = diff.count();
	
	return prog;
}

SlangHeader* SlangParser::ParseString(const std::string& code){
	tokenizer = std::make_unique<SlangTokenizer>(code,this);
	return Parse();
}

SlangHeader* SlangInterpreter::ParseSlangString(const SlangStr& code){
	if (!code.storage||code.storage->size==0){
		return (SlangHeader*)MakeMaybe(nullptr,false);
	}
	
	std::string_view sv{(const char*)code.storage->data,code.storage->size};
	
	parser.tokenizer = std::make_unique<SlangTokenizer>(sv,&parser);
	SlangHeader* res = parser.Parse();
	if (!parser.errors.empty())
		return (SlangHeader*)MakeMaybe(nullptr,false);
	return (SlangHeader*)MakeMaybe(res,true);
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
	
	codeSize = 65536;
	codeStart = (uint8_t*)malloc(codeSize);
	codePointer = codeStart;
	codeEnd = codeStart + codeSize;
}

SlangParser::~SlangParser(){
	free(codeStart);
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
			os << ((SlangObj*)&obj)->real;
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
			SlangLambda* lam = (SlangLambda*)&obj;
			bool printBracket = !variadic||lam->params->size>1;
			os << "(& ";
			if (printBracket)
				os << "(";
			
			if (lam->params){
				for (size_t i=0;i<lam->params->size;++i){
					if (i==lam->params->size-1&&i!=0&&variadic)
						os << " . ";
					else if (i!=0)
						os << ' ';
					os << gDebugParser->GetSymbolString(lam->params->params[i]);
				}
			}
			
			if (printBracket)
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
		case SlangType::Bool:
			if (obj.boolVal){
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
