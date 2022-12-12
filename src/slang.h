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
	
	struct SlangLambda;

	struct Env {
		std::map<SymbolName,SlangObj*> symbolMap;
		std::vector<Env*> children;
		Env* parent = nullptr;
		
		inline Env* MakeChild(){
			Env* child = new Env;
			*child = {{},{},this};
			children.push_back(child);
			return children.back();
		}
		
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
	
	SlangObj* AllocateObj();
	void FreeObj(SlangObj*);
	
	SlangObj MakeInt(int64_t);
	SlangObj MakeSymbol(SymbolName);
	SlangObj* MakeList(std::vector<SlangObj>&);
		
	typedef std::string::const_iterator StringIt;
	
	void ResetParseState();
	void Parse(const std::string&);
	
	std::ostream& operator<<(std::ostream&,SlangObj);
}
