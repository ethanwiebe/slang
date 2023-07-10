#pragma once

#include <stdint.h>
#include <string>
#include <unordered_map>
#include <map>
#include <vector>
#include <ostream>

#define SMALL_SET_SIZE (1365*24)
#define MEDIUM_SET_SIZE (5461*24)

namespace slang {
	typedef uint64_t SymbolName;
	typedef uint64_t LambdaKey;
	typedef std::unordered_map<std::string,SymbolName> SymbolNameDict;
	
	enum class SlangType : uint16_t {
		Null,
		List,
		Symbol,
		Lambda,
		Bool,
		Int,
		Real,
		String
	};
	
	enum SlangFlag : uint8_t {
		FLAG_FREE = 1,
		FLAG_MARKED = 2
	};
	
	inline bool IsNumericType(SlangType t){
		return t==SlangType::Int || t==SlangType::Real;
	}
	
	inline bool IsListType(SlangType t){
		return t==SlangType::List || t==SlangType::Null;
	}

	struct SlangObj {
		SlangType type;
		uint8_t flags;
		union {
			struct {
				SlangObj* left;
				SlangObj* right;
			};
			SymbolName symbol;
			LambdaKey lambda;
			int64_t integer;
			bool boolean;
			double real;
			uint8_t character;
		};
		
		inline bool operator==(const SlangObj& other) const {
			if (type==other.type){
				if (!IsListType(type)){
					// union moment
					return integer==other.integer;
				}
			} else {
				if (type==SlangType::Int&&other.type==SlangType::Real){
					return ((double)integer)==other.real;
				}
				
				if (type==SlangType::Real&&other.type==SlangType::Int){
					return real==((double)other.integer);
				}
			}
			return false;
		}
	};
	
	struct MemArena {
		uint8_t smallSet[SMALL_SET_SIZE];
		uint8_t mediumSet[MEDIUM_SET_SIZE];
		
		uint8_t* smallPointer = &smallSet[0];
		uint8_t* mediumPointer = &mediumSet[0];
		
		uint8_t* smallSetEnd = &smallSet[0]+sizeof(smallSet);
		uint8_t* mediumSetEnd = &mediumSet[0]+sizeof(mediumSet);
		
		inline bool InSmallSet(const SlangObj* obj) const {
			return (void*)obj>=(void*)&smallSet[0] && (void*)obj<(void*)smallSetEnd;
		}
	};
		
	struct Env {
		std::map<SymbolName,SlangObj*> symbolMap;
		std::vector<Env*> children;
		Env* parent = nullptr;
		
		inline void Clear();
		void DefSymbol(SymbolName,SlangObj*);
		// symbol might not exist
		SlangObj* GetSymbol(SymbolName) const;
	};
	
	struct SlangLambda {
		std::vector<SymbolName> params;
		SlangObj* body;
		Env* env;
		bool variadic;
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
		SlangObj* ParseExpr();
		SlangObj* ParseObj();
		SlangObj* ParseExprOrObj();
		//inline SlangObj* WrapProgram(const std::vector<SlangObj*>&);
		SlangObj* LoadIntoBuffer(SlangObj* prog);
		
		SlangObj* Parse(const std::string& code);
	};
	
	struct SlangInterpreter {
		SlangParser parser;
		Env env;
		SlangObj* program;
		std::vector<SlangLambda> globalLambdas;
		MemArena* arena;
		std::vector<SlangObj> stack;
		size_t baseIndex,frameIndex;
		std::vector<size_t> frameStack;
		
		std::vector<ErrorData> errors;
		
		inline LambdaKey RegisterLambda(const SlangLambda& lam){
			globalLambdas.push_back(lam);
			return globalLambdas.size()-1;
		}
		
		inline SlangLambda* GetLambda(LambdaKey key){
			return &globalLambdas.at(key);
		}
		
		inline bool SlangFuncDefine(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncLambda(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncSet(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncQuote(SlangObj* args,SlangObj* res);
		inline bool SlangFuncNot(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncNegate(SlangObj* args,SlangObj* res,Env* env);
		
		inline bool SlangFuncInc(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncDec(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncAdd(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncSub(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncMul(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncDiv(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncMod(SlangObj* args,SlangObj* res,Env* env);
		
		inline bool SlangFuncPair(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncLeft(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncRight(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncSetLeft(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncSetRight(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncPrint(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncAssert(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncEq(SlangObj* args,SlangObj* res,Env* env);
		inline bool SlangFuncIs(SlangObj* args,SlangObj* res,Env* env);
		
		inline bool WrappedEvalExpr(SlangObj* expr,SlangObj* res,Env* env);
		bool EvalExpr(SlangObj* expr,SlangObj* res,Env* env);
		
		inline SlangObj* StackAlloc();
		inline void StackAllocFrame();
		inline void StackFreeFrame();
		
		//void Reset();
		void PushError(const SlangObj*,const std::string&);
		void EvalError(const SlangObj*);
		void ProcError(const SlangObj*);
		void TypeError(const SlangObj*,SlangType found,SlangType expected);
		void AssertError(const SlangObj*);
		void ArityError(const SlangObj* head,size_t found,size_t expected);
		
		SlangObj* Parse(const std::string& code);
		void Run(SlangObj* prog);
		
		bool Validate(SlangObj* prog);
		
		SlangObj* AllocateObj();
		inline void MarkObjs(Env*);
		inline void FreeObj(SlangObj*);
		inline void FreeExpr(SlangObj*);
		inline void FreeEnv(Env*);
		inline SlangObj* Copy(const SlangObj*);
		inline Env* MakeEnvChild(Env*);
		
		inline SlangObj* MakeInt(int64_t);
		inline SlangObj* MakeBool(bool);
		inline SlangObj* MakeSymbol(SymbolName);
		inline SlangObj* MakeList(const std::vector<SlangObj*>&);
	};
	
	extern SlangParser* gDebugParser;
	extern SlangInterpreter* gDebugInterpreter;
	
	std::ostream& operator<<(std::ostream&,SlangObj);
}
