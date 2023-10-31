#pragma once

#include <stdint.h>
#include <string>
#include <unordered_map>
#include <map>
#include <vector>
#include <list>
#include <ostream>
#include <memory>
#include <assert.h>
#include <functional>

#define SMALL_SET_SIZE 32768
#define LARGE_SET_SIZE SMALL_SET_SIZE*4
#define FORWARD_MASK (~7ULL)
#define SLANG_ENV_BLOCK_SIZE 4

namespace slang {
	typedef uint64_t SymbolName;
	typedef std::unordered_map<std::string,SymbolName> SymbolNameDict;
	
	void PrintInfo();
	
	enum class SlangType : uint8_t {
		NullType,
		
		List,
		Symbol,
		Lambda,
		Bool,
		Int,
		Real,
		Vector,
		String,
		InputStream,
		OutputStream,
		EndOfFile,
		Maybe,
		// inaccessible
		Env,
		Params,
		Storage,
	};
	
	enum SlangFlag {
		FLAG_FORWARDED =          0b1,
		FLAG_VARIADIC =        0b1000,
		FLAG_MAYBE_OCCUPIED = 0b10000
	};
	
	struct SlangHeader {
		union {
			struct {
				uint8_t flags;
				SlangType type;
				union {
					uint16_t varCount;
					uint16_t elemSize;
					uint8_t isFile;
					uint8_t boolVal;
					uint8_t padding[6];
				};
			};
			SlangHeader* forwarded;
		};
		
		inline void Forward(SlangHeader* f){
			// sets the lowest bit of forwarding address
			forwarded = (SlangHeader*)((uint64_t)f | 1ULL);
		}
		
		inline bool IsForwarded() const {
			return (uint64_t)forwarded & 1ULL;
		}
		
		inline SlangHeader* GetForwardAddress() const {
			return (SlangHeader*)((uint64_t)forwarded & (uint64_t)FORWARD_MASK);
		}
		
		inline size_t GetSize() const;
	};
	
	static_assert(sizeof(SlangHeader)==8);
	
	struct SlangMapping {
		SymbolName sym;
		SlangHeader* obj;
	};
	
	struct SlangStorage {
		SlangHeader header;
		// measured in bytes
		size_t size;
		size_t capacity;
		union {
			struct {
				uint8_t data[];
			};
			struct {
				SlangHeader* objs[];
			};
		};
	};
	
	inline size_t GetStorageSize(const SlangStorage* s){
		if (!s) return 0;
		return s->size;
	}
	
	inline size_t GetStorageCapacity(const SlangStorage* s){
		if (!s) return 0;
		return s->capacity;
	}
	
	inline size_t GetStorageFreeSpace(const SlangStorage* s){
		if (!s) return 0;
		return s->capacity - s->size;
	}
	
	static_assert(sizeof(SlangStorage)==24);
	
	struct SlangStr {
		SlangHeader header;
		SlangStorage* storage;
		
		inline void CopyFromString(const std::string& s){
			assert(s.size()==storage->size);
			for (size_t i=0;i<storage->size;++i){
				storage->data[i] = s[i];
			}
		}
		
		inline size_t GetLength() const {
			return (!storage) ? 0 : storage->size;
		}
	};
	
	struct SlangVec {
		SlangHeader header;
		SlangStorage* storage;
		
		inline size_t GetLength() const {
			return (!storage) ? 0 : storage->size;
		}
	};
	
	struct SlangStream {
		SlangHeader header;
		size_t pos;
		union {
			SlangStr* str;
			FILE* file;
		};
		
		inline bool IsAtEnd() const {
			if (header.isFile){
				return feof(file);
			} else {
				return pos>=GetStorageSize(str->storage);
			}
		}
	};
	
	struct SlangEnv {
		SlangHeader header;
		SlangEnv* parent;
		SlangEnv* next;
		SlangMapping mappings[SLANG_ENV_BLOCK_SIZE];
		
		inline bool GetSymbol(SymbolName,SlangHeader**) const;
		inline bool DefSymbol(SymbolName,SlangHeader*);
		inline bool SetSymbol(SymbolName,SlangHeader*);
		inline void AddBlock(SlangEnv*);
	};
	
	struct SlangList {
		SlangHeader header;
		SlangHeader* left;
		SlangHeader* right;
	};
	
	struct SlangParams {
		SlangHeader header;
		size_t size;
		SymbolName params[];
	};
	
	struct SlangLambda {
		SlangHeader header;
		SlangHeader* expr;
		SlangParams* params;
		SlangEnv* env;
	};
	
	struct SlangObj {
		SlangHeader header;
		union {
			SymbolName symbol;
			int64_t integer;
			double real;
			SlangHeader* maybe;
		};
	};
	
	static_assert(sizeof(SlangObj)==16);
	struct SlangInterpreter;
	typedef bool(*SlangFunc)(SlangInterpreter* s,SlangHeader** res);
	
	#define VARIADIC_ARG_COUNT -1ULL
	
	struct ExternalFunc {
		const char* name;
		SlangFunc func;
		size_t minArgs = 0;
		size_t maxArgs = 0;
	};
	
	inline SlangType GetType(const SlangHeader* expr){
		if (!expr) return SlangType::NullType;
		return expr->type;
	}
	
	inline bool IsConstant(const SlangHeader* o){
		switch (GetType(o)){
			case SlangType::NullType:
			case SlangType::Int:
			case SlangType::Real:
			case SlangType::Env:
			case SlangType::Params:
			case SlangType::Lambda:
			case SlangType::InputStream:
			case SlangType::OutputStream:
			case SlangType::EndOfFile:
			case SlangType::Maybe:
			case SlangType::Bool:
			case SlangType::Vector:
			case SlangType::String:
			case SlangType::Storage:
				return true;
			case SlangType::List:
			case SlangType::Symbol:
				return false;
		}
		return false;
	}
	
	inline bool IsNumeric(const SlangHeader* o){
		return GetType(o)==SlangType::Int || GetType(o)==SlangType::Real;
	}
	
	inline bool IsList(const SlangHeader* o){
		return o==nullptr||o->type==SlangType::List;
	}
	
	inline size_t QuantizeSize(size_t size){
		return size+((-size)&7);
	}
	
	inline size_t QuantizeSize16(size_t size){
		return size+((-size)&15);
	}
	
	inline bool IsPrintable(uint8_t c){
		return c>=32 && c<=127;
	}
	
	struct MemArena {
		size_t memSize = 0;
		uint8_t* memSet = nullptr;
		
		uint8_t* currSet = nullptr;
		uint8_t* otherSet = nullptr;
		
		uint8_t* currPointer = nullptr;
		
		inline void SetSpace(uint8_t* ptr,size_t size,uint8_t* newCurr){
			assert((size&7) == 0);
			assert(((uint64_t)ptr&7) == 0);
			memSet = ptr;
			currSet = ptr;
			currPointer = newCurr;
			otherSet = ptr+size/2;
			memSize = size;
		}
		
		inline void SwapSets(){
			currPointer = otherSet;
			otherSet = currSet;
			currSet = currPointer;
		}
		
		inline bool InCurrSet(const uint8_t* obj) const {
			return obj>=&currSet[0] && 
					obj<(&currSet[0]+memSize/2);
		}
	};
	
	typedef std::function<void*(size_t)> AllocFunc;
	struct SlangAllocator {
		AllocFunc alloc;
		
		inline SlangObj* AllocateObj(SlangType);
		inline SlangLambda* AllocateLambda();
		inline SlangList* AllocateList();
		inline SlangParams* AllocateParams(size_t);
		inline SlangEnv* AllocateEnv();
		inline SlangStorage* AllocateStorage(size_t size,uint16_t elemSize);
		inline SlangStr* AllocateStr(size_t);
		inline SlangStream* AllocateStream(SlangType);
		
		inline SlangObj* MakeInt(int64_t);
		inline SlangObj* MakeReal(double);
		inline SlangObj* MakeSymbol(SymbolName);
		inline SlangHeader* MakeBool(bool);
		inline SlangParams* MakeParams(const std::vector<SymbolName>&);
		inline SlangHeader* MakeEOF();
	};
	
	typedef std::string_view::const_iterator StringIt;
	
	struct LocationData {
		uint32_t line,col;
		const char* filename;
	};
	
	struct ErrorData {
		LocationData loc;
		std::string message;
	};
	
	enum class SlangTokenType : uint8_t {
		Symbol = 0,
		Int,
		Real,
		String,
		LeftBracket,
		RightBracket,
		Quote,
		Not,
		Negation,
		Dot,
		Comment,
		True,
		False,
		EndOfFile,
		Error
	};
	
	struct SlangToken {
		SlangTokenType type;
		std::string_view view;
		uint32_t line,col;
	};
	
	struct SlangParser;
	
	struct SlangTokenizer {
		std::string_view tokenStr;
		SlangParser* parser;
		StringIt pos,end;
		uint32_t line,col;
		
		inline SlangTokenizer(std::string_view code,SlangParser* parser) : tokenStr(code),parser(parser){
			pos = tokenStr.cbegin();
			end = tokenStr.cend();
			line = 0;
			col = 0;
		}
		SlangToken NextToken();
	};
	
	struct SlangParser {
		std::unique_ptr<SlangTokenizer> tokenizer;
		SlangToken token;
		
		SymbolNameDict nameDict;
		SymbolName currentName;
		std::vector<ErrorData> errors;
		
		const char* currFilename;
		std::list<std::string> filenameList;
		
		SlangAllocator alloc;
		size_t codeSize = 0;
		size_t totalAlloc = 0;
		std::vector<uint8_t*> codeSets;
		uint8_t* codeStart = nullptr;
		uint8_t* codePointer = nullptr;
		uint8_t* codeEnd = nullptr;
		
		std::unordered_map<const SlangHeader*,LocationData> codeMap;
		
		SlangParser();
		~SlangParser();
		
		SlangParser(const SlangParser&) = delete;
		SlangParser& operator=(const SlangParser&) = delete;
		
		inline SlangList* WrapExprIn(SymbolName func,SlangHeader* expr);
		inline SlangList* WrapExprSplice(SymbolName func,SlangList* expr);
		inline SlangHeader* WrapProgram(const std::vector<SlangHeader*>&);
		inline void* CodeAlloc(size_t);
		inline void CodeRealloc(size_t);
		inline void PushFilename(const std::string& name);
		inline LocationData GetExprLocation(const SlangHeader*);
		
		std::string GetSymbolString(SymbolName name);
		SymbolName RegisterSymbol(const std::string& name);
		
		void PushError(const std::string& msg);
		
		inline void NextToken();
		bool ParseExpr(SlangHeader**);
		bool ParseObj(SlangHeader**);
		SlangHeader* LoadIntoBuffer(SlangHeader* prog);
		void TestSeqMacro(SlangList* list,size_t argCount);
		
		SlangHeader* ParseString(const std::string& code);
		SlangHeader* Parse();
	};
	
	typedef void(*FinalizerFunc)(SlangInterpreter* s,SlangHeader* obj);
	
	struct Finalizer {
		SlangHeader* obj;
		FinalizerFunc func;
	};
	
	struct SlangInterpreter {
		SlangParser parser;
		MemArena* arena;
		SlangAllocator alloc;
		SymbolName genIndex;
		
		size_t envIndex,exprIndex,argIndex,frameIndex,exportIndex;
		std::vector<SlangHeader*> funcArgStack;
		std::vector<SlangEnv*> envStack;
		std::vector<SlangHeader*> exprStack;
		std::vector<SlangList*> argStack;
		std::vector<Finalizer> finalizers;
		std::vector<SlangEnv*> exportStack;
		
		size_t filenameIndex;
		std::vector<std::string> filenameStack;
		
		std::vector<ErrorData> errors;
		std::map<SymbolName,ExternalFunc> extFuncs;
		
		SlangInterpreter();
		~SlangInterpreter();
		
		SlangInterpreter(const SlangInterpreter&) = delete;
		SlangInterpreter& operator=(const SlangInterpreter&) = delete;
		SlangInterpreter(SlangInterpreter&&) = delete;
		SlangInterpreter& operator=(SlangInterpreter&&) = delete;
		
		void AddExternalFunction(const ExternalFunc&);
		
		SlangHeader* ParseSlangString(const SlangStr&);
		
		inline SlangEnv* GetCurrEnv() const {
			return envStack[envIndex];
		}
		
		inline void PushEnv(SlangEnv* env);
		inline void PopEnv();
		
		inline void PushExportEnv();
		inline void PopExportEnv();
		
		void PushFilename(const std::string&);
		void PopFilename();
		
		void SetGlobalSymbol(const std::string& name,SlangHeader* value);
		bool GetGlobalSymbol(const std::string& name,SlangHeader** value);
		bool CallLambda(
			const SlangLambda* lam,
			const std::vector<SlangHeader*>& args,
			SlangHeader** res
		);
		
		inline SlangList* GetCurrArg() const {
			return argStack[argIndex];
		}
		
		inline SlangHeader* GetCurrExpr() const {
			return exprStack[exprIndex];
		}
		inline void PushArg(SlangList* expr);
		inline void PopArg();
		inline void PushExpr(SlangHeader* expr);
		inline void SetExpr(SlangHeader* expr);
		inline void PopExpr();
		
		inline void AddFinalizer(const Finalizer&);
		inline void RemoveFinalizer(SlangHeader*);
		
		inline bool NextArg(){
			SlangList* currArg = GetCurrArg();
			if (GetType(currArg->right)!=SlangType::List&&currArg->right!=nullptr){
				TypeError((SlangHeader*)currArg,GetType((SlangHeader*)currArg),SlangType::List);
				return false;
			}
			argStack[argIndex] = (SlangList*)currArg->right;
			return true;
		}
		
		inline bool SetRecursiveSymbol(SlangEnv*,SymbolName,SlangHeader*);
		
		inline bool AddReals(SlangHeader** res,double curr);
		inline bool MulReals(SlangHeader** res,double curr);
		bool GetLetParams(
			std::vector<SymbolName>& params,
			SlangList* paramIt
		);
		inline bool PreprocessApplyArgs(SlangList*,SlangList**);
		inline bool GetTwoIntArgs(SlangObj** l,SlangObj** r);
		inline bool MapInnerLoop(size_t,size_t,size_t,size_t&);
		
		inline bool SlangFuncDefine(SlangHeader** res);
		inline bool SlangFuncLambda(SlangHeader** res);
		inline bool SlangFuncSet(SlangHeader** res);
		inline bool SlangFuncMap(SlangHeader** res);
		inline bool SlangFuncQuote(SlangHeader** res);
		inline bool SlangFuncNot(SlangHeader** res);
		inline bool SlangFuncNegate(SlangHeader** res);
		inline bool SlangFuncLen(SlangHeader** res);
		
		inline bool SlangFuncInc(SlangHeader** res);
		inline bool SlangFuncDec(SlangHeader** res);
		inline bool SlangFuncAdd(SlangHeader** res);
		inline bool SlangFuncSub(SlangHeader** res);
		inline bool SlangFuncMul(SlangHeader** res);
		inline bool SlangFuncDiv(SlangHeader** res);
		inline bool SlangFuncFloorDiv(SlangHeader** res);
		inline bool SlangFuncMod(SlangHeader** res);
		inline bool SlangFuncPow(SlangHeader** res);
		inline bool SlangFuncAbs(SlangHeader** res);
		inline bool SlangFuncFloor(SlangHeader** res);
		inline bool SlangFuncCeil(SlangHeader** res);
		
		inline bool SlangFuncBitAnd(SlangHeader** res);
		inline bool SlangFuncBitOr(SlangHeader** res);
		inline bool SlangFuncBitXor(SlangHeader** res);
		inline bool SlangFuncBitNot(SlangHeader** res);
		inline bool SlangFuncBitLeftShift(SlangHeader** res);
		inline bool SlangFuncBitRightShift(SlangHeader** res);
		
		inline bool SlangFuncGT(SlangHeader** res);
		inline bool SlangFuncLT(SlangHeader** res);
		inline bool SlangFuncGTE(SlangHeader** res);
		inline bool SlangFuncLTE(SlangHeader** res);
		
		inline bool SlangFuncUnwrap(SlangHeader** res);
		inline bool SlangFuncList(SlangHeader** res);
		inline bool SlangFuncListGet(SlangHeader** res);
		inline bool SlangFuncListSet(SlangHeader** res);
		inline bool SlangFuncVec(SlangHeader** res);
		inline bool SlangFuncVecAlloc(SlangHeader** res);
		inline bool SlangFuncVecSize(SlangHeader** res);
		inline bool SlangFuncVecGet(SlangHeader** res);
		inline bool SlangFuncVecSet(SlangHeader** res);
		inline bool SlangFuncVecAppend(SlangHeader** res);
		inline bool SlangFuncVecPop(SlangHeader** res);
		inline bool SlangFuncStrGet(SlangHeader** res);
		inline bool SlangFuncStrSet(SlangHeader** res);
		inline bool SlangFuncMakeStrIStream(SlangHeader** res);
		inline bool SlangFuncMakeStrOStream(SlangHeader** res);
		inline bool SlangFuncStreamGetStr(SlangHeader** res);
		inline bool SlangFuncStreamWrite(SlangHeader** res);
		inline bool SlangFuncStreamRead(SlangHeader** res);
		inline bool SlangFuncStreamWriteByte(SlangHeader** res);
		inline bool SlangFuncStreamReadByte(SlangHeader** res);
		inline bool SlangFuncStreamSeekPrefix(SlangStream** stream,ssize_t* offset);
		inline bool SlangFuncStreamSeekBegin(SlangHeader** res);
		inline bool SlangFuncStreamSeekEnd(SlangHeader** res);
		inline bool SlangFuncStreamSeekOffset(SlangHeader** res);
		inline bool SlangFuncStreamTell(SlangHeader** res);
		
		inline bool SlangFuncOutputTo(SlangHeader** res);
		inline bool SlangFuncInputFrom(SlangHeader** res);
		
		inline bool SlangFuncPair(SlangHeader** res);
		inline bool SlangFuncLeft(SlangHeader** res);
		inline bool SlangFuncRight(SlangHeader** res);
		inline bool SlangFuncSetLeft(SlangHeader** res);
		inline bool SlangFuncSetRight(SlangHeader** res);
		inline bool SlangFuncPrint(SlangHeader** res);
		inline bool SlangFuncAssert(SlangHeader** res);
		inline bool SlangFuncEq(SlangHeader** res);
		inline bool SlangFuncIs(SlangHeader** res);
		
		inline bool SlangFuncExport(SlangHeader** res);
		inline bool SlangFuncImport(SlangHeader** res);
		
		inline bool SlangOutputToFile(FILE* file,SlangHeader* obj);
		inline SlangHeader* SlangInputFromFile(FILE* file);
		inline bool SlangOutputToString(SlangStream* stream,SlangHeader* obj);
		inline SlangHeader* SlangInputFromString(SlangStream* stream);
		
		inline bool WrappedEvalExpr(SlangHeader* expr,SlangHeader** res);
		bool EvalExpr(SlangHeader** res);
		
		inline void StackAddArg(SlangHeader*);
		
		void PushError(const SlangHeader*,const std::string&);
		void EvalError(const SlangHeader*);
		void ValueError(const SlangHeader*,const std::string&);
		void FileError(const SlangHeader*,const std::string&);
		void StreamError(const SlangHeader*,const std::string&);
		void ImportError(const SlangHeader*,const std::string&);
		void UnwrapError(const SlangHeader*);
		void IndexError(const SlangHeader*,size_t,ssize_t);
		void UndefinedError(const SlangHeader*);
		void RedefinedError(const SlangHeader*,SymbolName);
		void ProcError(const SlangHeader*);
		void TypeError(const SlangHeader*,SlangType found,SlangType expected,size_t index=-1ULL);
		void AssertError(const SlangHeader*);
		void ArityError(const SlangHeader* head,size_t found,size_t expectMin,size_t expectMax);
		void ZeroDivisionError(const SlangHeader*);
		
		SlangHeader* Parse(const std::string& code);
		bool Run(SlangHeader* prog,SlangHeader** res);
		
		inline void* Allocate(size_t);
		inline SlangStorage* MakeStorage(size_t,uint16_t,SlangHeader* fill=nullptr);
		inline SlangEnv* AllocateEnvs(size_t);
		inline SlangStr* ReallocateStr(SlangStr*,size_t);
		inline SlangStream* ReallocateStream(SlangStream*,size_t);
		inline void ReallocSet(size_t newSize);
		inline void SmallGC(size_t);
		
		inline SlangHeader* Copy(SlangHeader*);
		
		inline void DefEnvSymbol(SlangEnv* e,SymbolName name,SlangHeader* val);
		inline void ImportEnv(SlangEnv* e);
		inline bool GetImportName(SlangList* nameList,std::string& path);
		inline SymbolName GenSym();
		inline SlangEnv* CreateEnv(size_t);
		
		inline SlangObj* MakeMaybe(SlangHeader*,bool);
		inline SlangLambda* MakeLambda(
			SlangHeader* expr,
			const std::vector<SymbolName>& args,
			bool variadic,
			bool extraSlot=false);
		inline SlangStream* MakeStringInputStream(SlangStr*);
		inline SlangStream* MakeStringOutputStream(SlangStr*);
		inline SlangList* MakeList(const std::vector<SlangHeader*>&,size_t start=0,size_t end=-1ULL);
		inline void ConcatLists(SlangList* a,SlangList* b) const;
		inline SlangVec* MakeVecFromVec(const std::vector<SlangHeader*>&,size_t start=0,size_t end=-1ULL);
		inline SlangVec* MakeVec(size_t,SlangHeader*);
	};
	
	extern SlangParser* gDebugParser;
	extern SlangInterpreter* gDebugInterpreter;
	
	std::ostream& operator<<(std::ostream&,const SlangHeader&);
}
