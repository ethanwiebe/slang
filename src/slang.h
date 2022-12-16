#pragma once

#include <stdint.h>
#include <string>
#include <unordered_map>
#include <map>
#include <vector>
#include <ostream>

namespace slang {
	typedef uint64_t SymbolName;
	typedef uint64_t LambdaKey;
	
	typedef std::unordered_map<std::string,SymbolName> SymbolNameDict;
	extern SymbolNameDict gSymbolDict;
	extern SymbolName gCurrentSymbolName;
	
	enum class SlangType : uint8_t {
		Null,
		List,
		Symbol,
		Lambda,
		Bool,
		Int,
		Real,
		String
	};
	
	struct SlangObj {
		SlangType type;
		
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
	};
	
	struct Env {
		std::map<SymbolName,SlangObj*> symbolMap;
		std::vector<Env*> children;
		Env* parent = nullptr;
		
		inline Env* MakeChild();
		
		inline void Clear();
		void DefSymbol(SymbolName,SlangObj*);
		// symbol might not exist
		SlangObj* GetSymbol(SymbolName) const;
	};
	
	struct SlangLambda {
		std::vector<SymbolName> params;
		SlangObj* body;
		Env* env;
	};
	
	extern std::vector<SlangLambda> gLambdaDict;
	
	SymbolName RegisterSymbol(const std::string& name);
	LambdaKey RegisterLambda(const SlangLambda&);
	std::string GetSymbolString(SymbolName symbol);
	
	SlangObj* EvalExpr(const SlangObj* expr,Env& env);
	
	typedef std::string::const_iterator StringIt;
	
	struct ParseErrorData {
		uint32_t line,col;
		std::string message;
	};
	
	enum class SlangTokenType : uint8_t {
		Symbol = 0,
		Int,
		Real,
		LeftBracket,
		RightBracket,
		Quote,
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
		
		SlangTokenizer(const std::string& code) : 
			tokenStr(code),
			pos(tokenStr.begin()),
			end(tokenStr.end()),
			line(),col(){}
		
		SlangToken NextToken();
	};
	
	struct SlangParser {
		SlangTokenizer tokenizer;
		SlangToken token;
		
		SymbolNameDict nameDict;
		SymbolName currentName;
		std::vector<ParseErrorData> errors;
		
		SlangParser(const std::string& code) : 
			tokenizer(code),token(),nameDict(),currentName(),errors(){
			token.type = SlangTokenType::Comment;
		}
		
		std::string GetSymbolString(SymbolName name);
		SymbolName RegisterSymbol(const std::string& name);
		
		void PushError(const std::string& msg);
		
		inline void NextToken();
		SlangObj* ParseExpr();
		SlangObj* ParseObj();
		SlangObj* ParseExprOrObj();
		SlangObj* Parse();
	};
	
	typedef std::vector<SlangObj*> SlangArgVec;
	typedef std::vector<SlangObj*> GarbageVec;
	
	struct SlangInterpreter {
		SlangObj* program;
		Env env;
		std::vector<SlangLambda> globalLambdas;
		GarbageVec garbage;
		
		inline LambdaKey RegisterLambda(const SlangLambda& lam){
			globalLambdas.push_back(lam);
			return globalLambdas.size()-1;
		}
		
		inline SlangLambda* GetLambda(LambdaKey key){
			return &globalLambdas.at(key);
		}
		
		inline SlangObj* SlangFuncDefine(const SlangArgVec& args,Env* env);
		inline SlangObj* SlangFuncLambda(const SlangArgVec& args,Env* env);
		inline SlangObj* SlangFuncSet(const SlangArgVec& args,Env* env);
		inline SlangObj* SlangFuncQuote(const SlangArgVec& args);
		inline SlangObj* SlangFuncAdd(const SlangArgVec& args,Env* env);
		inline SlangObj* SlangFuncSub(const SlangArgVec& args,Env* env);
		inline SlangObj* SlangFuncMul(const SlangArgVec& args,Env* env);
		inline SlangObj* SlangFuncPair(const SlangArgVec& args,Env* env);
		inline SlangObj* SlangFuncLeft(const SlangArgVec& args,Env* env);
		inline SlangObj* SlangFuncRight(const SlangArgVec& args,Env* env);
		//inline SlangObj* SlangFuncSetLeft(const SlangArgVec& args,Env* env);
		//inline SlangObj* SlangFuncSetRight(const SlangArgVec& args,Env* env);
		
		SlangObj* EvalExpr(const SlangObj* expr,Env* env);
		
		void Reset();
		void Run(SlangObj* prog);
	};
	
	extern SlangParser* gDebugParser;
	extern SlangInterpreter* gDebugInterpreter;
	
	std::ostream& operator<<(std::ostream&,SlangObj);
}
