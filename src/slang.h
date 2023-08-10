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

#define SMALL_SET_SIZE 32768
#define LARGE_SET_SIZE SMALL_SET_SIZE*4
#define FORWARD_MASK (~7ULL)
#define SLANG_ENV_BLOCK_SIZE 4

namespace slang {
	typedef uint64_t SymbolName;
	typedef std::unordered_map<std::string,SymbolName> SymbolNameDict;
	
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
		// inaccessible
		Env,
		Params,
		Storage,
	};
	
	enum SlangFlag {
		FLAG_FORWARDED =       0b1,
		FLAG_TENURED =        0b10,
		FLAG_BOOL_VALUE =    0b100,
		FLAG_VARIADIC =     0b1000
	};
	
	struct SlangHeader {
		union {
			struct {
				uint8_t flags;
				SlangType type;
				union {
					uint16_t varCount;
					uint16_t elemSize;
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
	
	struct SlangEnv {
		SlangHeader header;
		SlangEnv* parent;
		SlangEnv* next;
		SlangMapping mappings[SLANG_ENV_BLOCK_SIZE];
		
		inline bool GetSymbol(SymbolName,SlangHeader**);
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
			bool boolean;
		};
	};
	
	static_assert(sizeof(SlangObj)==16);
	struct SlangInterpreter;
	typedef bool(*SlangFunc)(SlangInterpreter* s,SlangHeader** res);
	
	struct ExternalFunc {
		const char* name;
		SlangFunc func;
	};
	
	inline SlangType GetType(const SlangHeader* expr){
		if (!expr) return SlangType::NullType;
		return expr->type;
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
		
		//std::list<SlangObj*> tenureSets = {};
		//SlangObj* currTenureSet = nullptr;
		//SlangObj* currTenurePointer = nullptr;
		
		inline void SetSpace(uint8_t* ptr,size_t size){
			assert((size&7) == 0);
			assert(((uint64_t)ptr&7) == 0);
			memSet = ptr;
			currSet = ptr;
			currPointer = ptr;
			otherSet = ptr+size/2;
			memSize = size;
		}
		
		inline void SwapSets(){
			currPointer = otherSet;
			otherSet = currSet;
			currSet = currPointer;
		}
		/*
		inline SlangObj* TenureObj(){
			if (!currTenureSet){
				currTenureSet = (SlangObj*)malloc(LARGE_SET_SIZE*sizeof(SlangObj));
				tenureSets.push_back(currTenureSet);
				currTenurePointer = currTenureSet;
			}
			
			SlangObj* ptr = currTenurePointer++;
			if (currTenurePointer-currTenureSet > LARGE_SET_SIZE){
				currTenureSet = nullptr;
			}
			return ptr;
		}
		
		inline void ClearTenuredSets(){
			for (auto* ptr : tenureSets){
				free(ptr);
			}
			tenureSets.clear();
		}*/
		/*
		inline bool SameTenureSet(const SlangObj* s1,const SlangObj* s2) const {
			if (s1==s2) return true;
			if (std::abs(s1-s2)>LARGE_SET_SIZE) return false;
			for (auto it=tenureSets.rbegin();it!=tenureSets.rend();++it){
				if (s1<*it || s1>=(*it+LARGE_SET_SIZE)) continue;
				if (s2<*it || s2>=(*it+LARGE_SET_SIZE)) return false;
				return true;
			}
			return false;
		}*/
		
		inline bool InCurrSet(const uint8_t* obj) const {
			return obj>=&currSet[0] && 
					obj<(&currSet[0]+memSize/2);
		}
	};
	
	/*struct Env {
		std::vector<SymbolName> params;
		std::map<SymbolName,SlangHeader*> symbolMap;
		Env* parent = nullptr;
		bool variadic;
		bool marked;
		
		inline bool IsGlobal() const {
			return parent==nullptr;
		}
		
		inline void Clear();
		bool DefSymbol(SymbolName,SlangHeader*);
		// symbol might not exist
		SlangObj** GetSymbol(SymbolName);
	};*/
	
	typedef std::string_view::const_iterator StringIt;
	
	struct LocationData {
		uint32_t line,col;
		uint32_t fileIndex;
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
	
	struct SlangTokenizer {
		std::string_view tokenStr;
		StringIt pos,end;
		uint32_t line,col;
		
		inline SlangTokenizer(std::string_view code) : tokenStr(code){
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
		
		size_t codeSize = 0;
		uint8_t* codeStart = nullptr;
		uint8_t* codePointer = nullptr;
		uint8_t* codeEnd = nullptr;
		
		size_t codeCount;
		size_t codeIndex;
		
		std::unordered_map<const SlangHeader*,LocationData> codeMap;
		
		SlangParser();
		~SlangParser();
		
		SlangParser(const SlangParser&) = delete;
		SlangParser& operator=(const SlangParser&) = delete;
		
		inline SlangList* WrapExprIn(SymbolName func,SlangHeader* expr);
		inline SlangList* WrapExprSplice(SymbolName func,SlangList* expr);
		inline SlangHeader* WrapProgram(const std::vector<SlangHeader*>&);
		inline void* CodeAlloc(size_t);
		SlangObj* CodeAllocObj();
		SlangStr* CodeAllocStr(size_t);
		SlangList* CodeAllocList();
		
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
		SlangHeader* ParseSlangString(const SlangStr&);
		SlangHeader* Parse();
	};
	
	struct SlangInterpreter {
		SlangParser parser;
		SlangHeader* program;
		MemArena* arena;
		bool gcParity;
		size_t envIndex,exprIndex,argIndex,frameIndex;
		
		SymbolName genIndex;
		
		std::vector<SlangHeader*> funcArgStack;
		std::vector<SlangEnv*> envStack;
		std::vector<SlangHeader*> exprStack;
		std::vector<SlangList*> argStack;
		
		std::vector<ErrorData> errors;
		std::map<SymbolName,SlangFunc> extFuncs;
		
		
		SlangInterpreter();
		void AddExternalFunction(const ExternalFunc&);
		
		inline SlangEnv* GetCurrEnv() const {
			return envStack[envIndex];
		}
		
		inline void PushEnv(SlangEnv* env);
		inline void PopEnv();
		
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
			std::vector<SlangHeader*>& exprs,
			SlangList* paramIt
		);
		
		inline bool SlangFuncDefine(SlangHeader** res);
		inline bool SlangFuncLambda(SlangHeader** res);
		inline bool SlangFuncSet(SlangHeader** res);
		inline bool SlangFuncQuote(SlangHeader** res);
		inline bool SlangFuncNot(SlangHeader** res);
		inline bool SlangFuncNegate(SlangHeader** res);
		
		inline bool SlangFuncInc(SlangHeader** res);
		inline bool SlangFuncDec(SlangHeader** res);
		inline bool SlangFuncAdd(SlangHeader** res);
		inline bool SlangFuncSub(SlangHeader** res);
		inline bool SlangFuncMul(SlangHeader** res);
		inline bool SlangFuncDiv(SlangHeader** res);
		inline bool SlangFuncMod(SlangHeader** res);
		
		inline bool SlangFuncList(SlangHeader** res);
		inline bool SlangFuncVec(SlangHeader** res);
		inline bool SlangFuncVecAlloc(SlangHeader** res);
		inline bool SlangFuncVecSize(SlangHeader** res);
		inline bool SlangFuncVecGet(SlangHeader** res);
		inline bool SlangFuncVecSet(SlangHeader** res);
		inline bool SlangFuncPair(SlangHeader** res);
		inline bool SlangFuncLeft(SlangHeader** res);
		inline bool SlangFuncRight(SlangHeader** res);
		inline bool SlangFuncSetLeft(SlangHeader** res);
		inline bool SlangFuncSetRight(SlangHeader** res);
		inline bool SlangFuncPrint(SlangHeader** res);
		inline bool SlangFuncAssert(SlangHeader** res);
		inline bool SlangFuncEq(SlangHeader** res);
		inline bool SlangFuncIs(SlangHeader** res);
		
		inline bool WrappedEvalExpr(SlangHeader* expr,SlangHeader** res);
		bool EvalExpr(SlangHeader** res);
		
		inline void StackAddArg(SlangHeader*);
		
		//void Reset();
		void PushError(const SlangHeader*,const std::string&);
		void EvalError(const SlangHeader*);
		void IndexError(const SlangHeader*,size_t,ssize_t);
		void UndefinedError(const SlangHeader*);
		void RedefinedError(const SlangHeader*,SymbolName);
		void ProcError(const SlangHeader*);
		void TypeError(const SlangHeader*,SlangType found,SlangType expected,size_t index=-1ULL);
		void AssertError(const SlangHeader*);
		void ArityError(const SlangHeader* head,size_t found,size_t expected);
		void ZeroDivisionError(const SlangHeader*);
		
		SlangHeader* Parse(const std::string& code);
		void Run(SlangHeader* prog);
		
		inline void* Allocate(size_t);
		inline SlangObj* AllocateSmallObj();
		inline SlangStorage* AllocateStorage(size_t,uint16_t,SlangHeader* fill=nullptr);
		inline SlangVec* AllocateVec(size_t,SlangHeader* fill=nullptr);
		inline SlangStr* AllocateStr(size_t);
		inline SlangEnv* AllocateEnv(size_t);
		inline SlangList* AllocateList();
		inline SlangLambda* AllocateLambda();
		inline SlangParams* AllocateParams(size_t);
		inline SlangHeader* Evacuate(uint8_t** write,SlangHeader* obj);
		inline void EvacuateOrForward(uint8_t** write,SlangHeader** obj);
		inline void ForwardObj(SlangHeader* obj);
		//inline SlangObj* TenureEntireObj(SlangObj** write,SlangObj* obj);
		inline void ReallocSet(size_t newSize);
		inline void EvacuateEnv(uint8_t** write,SlangEnv* env);
		inline void ScavengeObj(uint8_t** write,SlangHeader* read);
		//inline void ScavengeTenure(SlangObj** write,SlangObj* start,SlangObj* end);
		inline void Scavenge(uint8_t** write,uint8_t* read);
		inline void DeleteEnv(SlangEnv* env);
		inline void EnvCleanup();
		inline void SmallGC(size_t);
		
		inline SlangHeader* Copy(SlangHeader*);
		
		inline void DefEnvSymbol(SymbolName name,SlangHeader* val);
		inline SymbolName GenSym();
		inline SlangEnv* CreateEnv(size_t);
		
		inline SlangObj* MakeInt(int64_t);
		inline SlangObj* MakeReal(double);
		inline SlangObj* MakeBool(bool);
		inline SlangParams* MakeParams(const std::vector<SymbolName>&);
		inline SlangLambda* MakeLambda(
			SlangHeader* expr,
			const std::vector<SymbolName>& args,
			bool variadic,
			bool extraSlot=false);
		inline SlangObj* MakeSymbol(SymbolName);
		inline SlangList* MakeList(const std::vector<SlangHeader*>&,size_t start=0,size_t end=-1ULL);
		inline SlangVec* MakeVec(const std::vector<SlangHeader*>&,size_t start=0,size_t end=-1ULL);
	};
	
	extern SlangParser* gDebugParser;
	extern SlangInterpreter* gDebugInterpreter;
	
	std::ostream& operator<<(std::ostream&,const SlangHeader&);
}
