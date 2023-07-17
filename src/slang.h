#pragma once

#include <stdint.h>
#include <string>
#include <unordered_map>
#include <map>
#include <vector>
#include <list>
#include <ostream>
#include <assert.h>

#define SMALL_SET_SIZE 1365
#define LARGE_SET_SIZE 10922

namespace slang {
	typedef uint64_t SymbolName;
	typedef std::unordered_map<std::string,SymbolName> SymbolNameDict;
	
	enum class SlangType : uint16_t {
		NullType,
		List,
		Symbol,
		Lambda,
		Bool,
		Int,
		Real,
		String
	};
	
	enum SlangFlag : uint8_t {
		FLAG_SURVIVE_MASK = 0b11,
		FLAG_FORWARDED = 4,
		FLAG_TENURED = 8
	};
	
	struct Env;
	
	struct SlangObj {
		SlangType type;
		uint8_t flags;
		union {
			struct {
				SlangObj* left;
				SlangObj* right;
			};
			struct {
				SlangObj* expr;
				Env* env;
			};
			SymbolName symbol;
			int64_t integer;
			bool boolean;
			double real;
			uint8_t character;
			SlangObj* forwarded;
		};
	};
	
	static_assert(sizeof(SlangObj)==24);
	
	inline SlangType GetType(const SlangObj* expr){
		if (!expr) return SlangType::NullType;
		return expr->type;
	}
	
	inline SlangObj* NextArg(const SlangObj* expr){
		return expr->right;
	}
	
	inline bool IsNumeric(const SlangObj* o){
		return GetType(o)==SlangType::Int || GetType(o)==SlangType::Real;
	}
	
	inline bool IsList(const SlangObj* o){
		return o==nullptr||o->type==SlangType::List;
	}
	
	struct MemArena {
		SlangObj leftSet[SMALL_SET_SIZE*sizeof(SlangObj)];
		SlangObj rightSet[SMALL_SET_SIZE*sizeof(SlangObj)];
		
		SlangObj* currSet = &leftSet[0];
		SlangObj* otherSet = &rightSet[0];
		
		SlangObj* currPointer = &leftSet[0];
		
		std::list<SlangObj*> tenureSets = {};
		SlangObj* currTenureSet = nullptr;
		SlangObj* currTenurePointer = nullptr;
		
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
		}
		
		inline bool SameTenureSet(const SlangObj* s1,const SlangObj* s2) const {
			if (s1==s2) return true;
			if (std::abs(s1-s2)>LARGE_SET_SIZE) return false;
			for (auto it=tenureSets.rbegin();it!=tenureSets.rend();++it){
				if (s1<*it || s1>=(*it+LARGE_SET_SIZE)) continue;
				if (s2<*it || s2>=(*it+LARGE_SET_SIZE)) return false;
				return true;
			}
			return false;
		}
		
		inline bool InCurrSet(const SlangObj* obj) const {
			return obj>=&currSet[0] && 
					obj<(&currSet[0]+SMALL_SET_SIZE);
		}
	};
		
	struct Env {
		std::vector<SymbolName> params;
		std::map<SymbolName,SlangObj*> symbolMap;
		Env* parent = nullptr;
		bool variadic;
		bool marked;
		
		inline bool IsGlobal() const {
			return parent==nullptr;
		}
		
		inline void Clear();
		bool DefSymbol(SymbolName,SlangObj*);
		// symbol might not exist
		SlangObj** GetSymbol(SymbolName);
	};
	
	typedef std::string::const_iterator StringIt;
	
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
		std::string tokenStr;
		StringIt pos,end;
		uint32_t line,col;
		
		inline void SetText(const std::string& code){
			tokenStr = code;
			pos = tokenStr.begin();
			end = tokenStr.end();
			line = 0;
			col = 0;
		}
		SlangToken NextToken();
	};
	
	struct SlangParser {
		SlangTokenizer tokenizer;
		SlangToken token;
		
		SymbolNameDict nameDict;
		SymbolName currentName;
		std::vector<ErrorData> errors;
		
		std::vector<SlangObj> codeBuffer;
		size_t codeCount;
		size_t codeIndex;
		
		std::unordered_map<const SlangObj*,LocationData> codeMap;
		
		SlangParser();
		
		inline LocationData GetExprLocation(const SlangObj*);
		
		std::string GetSymbolString(SymbolName name);
		SymbolName RegisterSymbol(const std::string& name);
		
		void PushError(const std::string& msg);
		
		inline void NextToken();
		bool ParseExpr(SlangObj**);
		bool ParseObj(SlangObj**);
		SlangObj* LoadIntoBuffer(SlangObj* prog);
		void TestSeqMacro(SlangObj* list,size_t argCount);
		
		SlangObj* Parse(const std::string& code);
	};
	
	struct SlangInterpreter {
		SlangParser parser;
		Env env;
		Env* currEnv;
		SlangObj* program;
		MemArena* arena;
		bool gcParity;
		size_t baseIndex,frameIndex;
		
		SymbolName genIndex;
		
		std::vector<SlangObj*> funcArgStack;
		std::vector<size_t> frameStack;
		
		std::vector<ErrorData> errors;
		
		inline bool AddReals(SlangObj* args,SlangObj** res,Env* env,double curr);
		inline bool MulReals(SlangObj* args,SlangObj** res,Env* env,double curr);
		bool GetLetParams(
			std::vector<SymbolName>& params,
			std::vector<SlangObj*>& exprs,
			SlangObj* paramIt
		);
		
		inline bool SlangFuncDefine(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncLambda(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncSet(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncQuote(SlangObj* args,SlangObj** res);
		inline bool SlangFuncNot(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncNegate(SlangObj* args,SlangObj** res,Env* env);
		
		inline bool SlangFuncInc(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncDec(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncAdd(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncSub(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncMul(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncDiv(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncMod(SlangObj* args,SlangObj** res,Env* env);
		
		inline bool SlangFuncPair(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncLeft(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncRight(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncSetLeft(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncSetRight(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncPrint(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncAssert(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncEq(SlangObj* args,SlangObj** res,Env* env);
		inline bool SlangFuncIs(SlangObj* args,SlangObj** res,Env* env);
		
		inline bool WrappedEvalExpr(SlangObj* expr,SlangObj** res,Env* env);
		bool EvalExpr(SlangObj* expr,SlangObj** res,Env* env);
		
		inline void StackAddArg(SlangObj*);
		inline void StackAllocFrame();
		inline void StackFreeFrame();
		
		//void Reset();
		void PushError(const SlangObj*,const std::string&);
		void EvalError(const SlangObj*);
		void UndefinedError(const SlangObj*);
		void RedefinedError(const SlangObj*,SymbolName);
		void ProcError(const SlangObj*);
		void TypeError(const SlangObj*,SlangType found,SlangType expected,size_t index=-1ULL);
		void AssertError(const SlangObj*);
		void ArityError(const SlangObj* head,size_t found,size_t expected);
		void ZeroDivisionError(const SlangObj*);
		
		SlangObj* Parse(const std::string& code);
		void Run(SlangObj* prog);
		
		inline SlangObj* AllocateObj();
		inline SlangObj* Evacuate(SlangObj** write,SlangObj* obj);
		inline SlangObj* TenureEntireObj(SlangObj** write,SlangObj* obj);
		inline void EvacuateEnv(SlangObj** write,Env* env);
		inline void ScavengeObj(SlangObj** write,SlangObj* read);
		inline void ScavengeTenure(SlangObj** write,SlangObj* start,SlangObj* end);
		inline void Scavenge(SlangObj** write,SlangObj* read);
		inline void DeleteEnv(Env* env);
		inline void EnvCleanup();
		inline void SmallGC();
		
		inline SlangObj* Copy(SlangObj*);
		
		inline SymbolName GenSym();
		inline Env* CreateEnv();
		
		inline SlangObj* MakeInt(int64_t);
		inline SlangObj* MakeReal(double);
		inline SlangObj* MakeBool(bool);
		inline SlangObj* MakeLambda(SlangObj* expr,Env* env,const std::vector<SymbolName>& args,bool variadic);
		inline SlangObj* MakeSymbol(SymbolName);
		inline SlangObj* MakeList(const std::vector<SlangObj*>&,size_t start=0,size_t end=-1ULL);
	};
	
	extern SlangParser* gDebugParser;
	extern SlangInterpreter* gDebugInterpreter;
	
	std::ostream& operator<<(std::ostream&,SlangObj);
}
