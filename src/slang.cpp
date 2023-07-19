#include "slang.h"

#include <assert.h>
#include <iostream>
#include <cstring>
#include <sstream>
#include <math.h>
#include <chrono>

#include <set>

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

inline SlangObj* SlangInterpreter::AllocateObj(){
	if (arena->currPointer<arena->currSet+arena->memSize/2){
		++gAllocCount;
		++gAllocTotal;
		SlangObj* p = arena->currPointer++;
		p->type = (SlangType)65;
		p->flags = 0;
		return p;
	}
	
	// gc curr set now
	SmallGC();
	if (arena->currPointer>=arena->currSet+arena->memSize/2){
		std::cout << "out of mem\n";
		assert(false);
		exit(1);
	}
	
	SlangObj* p = arena->currPointer++;
	
	++gAllocCount;
	++gAllocTotal;
	p->type = (SlangType)65;
	p->flags = 0;
	return p;
}

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
}

inline SlangObj* SlangInterpreter::Evacuate(SlangObj** write,SlangObj* obj){
	if (!obj) return nullptr;
	if (!arena->InCurrSet(obj)) return obj;
	
	//uint8_t flags = obj->flags;
	//uint8_t surviveCount = flags&FLAG_SURVIVE_MASK;
	SlangObj* toPtr;
	
	/*if (surviveCount>=2){
		toPtr = TenureEntireObj(write,obj);
	} else {*/
		//obj->flags = (flags&~FLAG_SURVIVE_MASK) | ((surviveCount+1)&FLAG_SURVIVE_MASK);
		toPtr = *write;
		*write += 1;
		memcpy(toPtr,obj,sizeof(SlangObj));
		
		obj->flags |= FLAG_FORWARDED;
		obj->forwarded = toPtr;
		obj->type = (SlangType)36;
	//}
	
	return obj->forwarded;
}

inline void SlangInterpreter::EvacuateEnv(SlangObj** write,Env* env){
	env->marked = gcParity;
	for (auto& [name,obj] : env->symbolMap){
		if (obj&&(obj->flags&FLAG_FORWARDED)){
			obj = obj->forwarded;
		} else {
			obj = Evacuate(write,obj);
		}
	}
	if (env->parent)
		EvacuateEnv(write,env->parent);
}

inline void SlangInterpreter::ScavengeObj(SlangObj** write,SlangObj* read){
	if (read->type==SlangType::List){
		if (read->left && read->left->flags & FLAG_FORWARDED){
			read->left = read->left->forwarded;
		} else {
			read->left = Evacuate(write,read->left);
		}
		if (read->right && read->right->flags & FLAG_FORWARDED){
			read->right = read->right->forwarded;
		} else {
			read->right = Evacuate(write,read->right);
		}
	} else if (read->type==SlangType::Lambda){
		if (read->expr && read->expr->flags & FLAG_FORWARDED){
			read->expr = read->expr->forwarded;
		} else {
			read->expr = Evacuate(write,read->expr);
		}
		EvacuateEnv(write,read->env);
	}
}

inline void SlangInterpreter::ScavengeTenure(SlangObj** write,SlangObj* start,SlangObj* end){
	while (start!=end){
		ScavengeObj(write,start);
		++start;
	}
}

inline void SlangInterpreter::Scavenge(SlangObj** write,SlangObj* read){
	while (read!=*write){
		ScavengeObj(write,read);
		++read;
	}
}

inline void SlangInterpreter::DeleteEnv(Env* env){
	delete env;
	--gEnvBalance;
}

inline void SlangInterpreter::EnvCleanup(){
	SlangObj* end = arena->currSet+arena->memSize/2;
	for (
			SlangObj* start = arena->currSet;
			start < end;
			start++){
		if (!(start->flags & FLAG_FORWARDED)){
			if (start->type==SlangType::Lambda){
				DeleteEnv(start->env);
				start->type = (SlangType)0;
			}
		}
	}
}

inline void SlangInterpreter::ReallocSet(size_t newSize){
	SlangObj* newPtr = (SlangObj*)malloc(newSize*sizeof(SlangObj));
	// copy over data
	size_t currPointerLen = arena->currPointer-arena->currSet;
	memcpy(newPtr,arena->currSet,currPointerLen*sizeof(SlangObj));
	SlangObj* newStart = newPtr;
	SlangObj* newCurrPointer = newPtr+currPointerLen;
	for (
			SlangObj* start=arena->currSet;
			start!=arena->currPointer;
			++start,++newStart
		){
		*newStart = *start;
		start->flags |= FLAG_FORWARDED;
		start->forwarded = newStart;
		start->type = (SlangType)52;
	}
	
	for (SlangObj* start=newPtr;start!=newCurrPointer;++start){
		if (start->type==SlangType::List){
			if (start->left && start->left->flags & FLAG_FORWARDED){
				start->left = start->left->forwarded;
			}
			if (start->right && start->right->flags & FLAG_FORWARDED){
				start->right = start->right->forwarded;
			}
		} else if (start->type==SlangType::Lambda){
			if (start->expr && start->expr-> flags & FLAG_FORWARDED){
				start->expr = start->expr->forwarded;
			}
			for (Env* e = start->env;e!=nullptr;e=e->parent){
				for (auto& [key,val] : e->symbolMap){
					if (val && val->flags & FLAG_FORWARDED){
						val = val->forwarded;
					}
				}
			}
		}
	}
	
	for (auto& [key,val] : env.symbolMap){
		if (val && val->flags & FLAG_FORWARDED){
			val = val->forwarded;
		}
	}
	
	for (size_t i=0;i<frameIndex;++i){
		if (funcArgStack[i] && funcArgStack[i]->flags & FLAG_FORWARDED){
			funcArgStack[i] = funcArgStack[i]->forwarded;
		}
	}
	
	free(arena->memSet);
	arena->SetSpace(newPtr,newSize);
	arena->currPointer = newCurrPointer;
	gMaxArenaSize = (newSize*sizeof(SlangObj) > gMaxArenaSize) ?
						newSize*sizeof(SlangObj) : gMaxArenaSize;
	++gReallocCount;
}

inline void SlangInterpreter::SmallGC(){
	gcParity = !gcParity;
	//SlangObj* tenureStart = arena->currTenurePointer;
	SlangObj* write = arena->otherSet;
	SlangObj* read = write;
	for (size_t i=0;i<frameIndex;++i){
		if (funcArgStack[i]->flags & FLAG_FORWARDED){
			funcArgStack[i] = funcArgStack[i]->left;
		} else {
			funcArgStack[i] = Evacuate(&write,funcArgStack[i]);
		}
	}
	
	//EvacuateEnv(&write,&env);
	if (currEnv)
		EvacuateEnv(&write,currEnv);
	
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
	
	EnvCleanup();
	arena->SwapSets();
	arena->currPointer = write;
	size_t spaceLeft = arena->currSet+arena->memSize/2-write;
	// less than 1/4 of the set left
	if (spaceLeft<arena->memSize/8){
		ReallocSet(arena->memSize*3/2);
	} else if (arena->memSize>SMALL_SET_SIZE*4&&spaceLeft>arena->memSize*7/16){
		// set more than 7/8 empty
		ReallocSet(arena->memSize/2);
	}
	++gSmallGCs;
}

SlangObj* SlangInterpreter::Copy(SlangObj* expr){
	StackAddArg(expr);
	SlangObj* n = AllocateObj();
	if (expr->flags & FLAG_FORWARDED){
		memcpy(n,expr->left,sizeof(SlangObj));
	} else {
		memcpy(n,expr,sizeof(SlangObj));
	}
	--frameIndex;
	return n;
}

inline bool IsIdentifierChar(char c){
	return (c>='A'&&c<='Z') ||
		   (c>='a'&&c<='z') ||
		   (c>='0'&&c<='9') ||
		   c=='_'||c=='-'||c=='?'||
		   c=='!'||c=='@'||c=='&'||
		   c=='*'||c=='+'||c=='/'||
		   c=='$'||c=='%'||c=='^'||
		   c=='~'||c=='=';
}

enum SlangGlobalSymbol {
	SLANG_DEFINE = 0,
	SLANG_LAMBDA,
	SLANG_LET,
	SLANG_SET,
	SLANG_IF,
	SLANG_DO,
	SLANG_QUOTE,
	SLANG_NOT,
	SLANG_NEG,
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
	GLOBAL_SYMBOL_COUNT
};

SymbolNameDict gDefaultNameDict = {
	{"def",SLANG_DEFINE},
	{"&",SLANG_LAMBDA},
	{"let",SLANG_LET},
	{"set!",SLANG_SET},
	{"if",SLANG_IF},
	{"do",SLANG_DO},
	{"quote",SLANG_QUOTE},
	{"not",SLANG_NOT},
	{"neg",SLANG_NEG},
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
};

bool Env::DefSymbol(SymbolName name,SlangObj* obj){
	bool ret = !symbolMap.contains(name);
	symbolMap.insert_or_assign(name,obj);
	return ret;
}

SlangObj** Env::GetSymbol(SymbolName name){
	if (!symbolMap.contains(name))
		return nullptr;
		
	return &symbolMap.at(name);
}

inline SymbolName SlangInterpreter::GenSym(){
	return genIndex++;
}

inline SlangObj* SlangInterpreter::MakeInt(int64_t i){
	SlangObj* obj = AllocateObj();
	obj->type = SlangType::Int;
	obj->integer = i;
	return obj;
}

inline SlangObj* SlangInterpreter::MakeReal(double r){
	SlangObj* obj = AllocateObj();
	obj->type = SlangType::Real;
	obj->real = r;
	return obj;
}

inline SlangObj* SlangInterpreter::MakeBool(bool b){
	SlangObj* obj = AllocateObj();
	obj->type = SlangType::Bool;
	obj->boolean = b;
	return obj;
}

inline SlangObj* SlangInterpreter::MakeSymbol(SymbolName symbol){
	SlangObj* obj = AllocateObj();
	obj->type = SlangType::Symbol;
	obj->symbol = symbol;
	return obj;
}

inline SlangObj* SlangInterpreter::MakeLambda(
				SlangObj* expr,
				Env* env,
				const std::vector<SymbolName>& args,
				bool variadic){
	SlangObj* obj = AllocateObj();
	obj->type = SlangType::Lambda;
	obj->expr = expr;
	obj->env = CreateEnv();
	obj->env->parent = env;
	obj->env->marked = gcParity;
	obj->env->params = args;
	obj->env->symbolMap = {};
	obj->env->variadic = variadic;
	
	return obj;
}

inline SlangObj* SlangInterpreter::MakeList(const std::vector<SlangObj*>& objs,size_t start,size_t end){
	SlangObj* listHead = nullptr;
	SlangObj* list = listHead;
	size_t m = std::min(end,objs.size());
	for (size_t i=start;i<m;++i){
		if (!list){
			list = AllocateObj();
			StackAddArg(list);
			// for GC purposes
			listHead = list;
		} else {
			SlangObj* newObj = AllocateObj();
			if (list->flags & FLAG_FORWARDED)
				list = list->left;
			list->right = newObj;
			list = list->right;
		}
		list->type = SlangType::List;
		list->left = objs[i];
		list->right = nullptr;
	}
	listHead = funcArgStack[frameIndex-1];
	--frameIndex;
	
	return listHead;
}

bool GCManualCollect(SlangInterpreter* s,SlangObj* args,SlangObj** res,Env*){
	if (args!=nullptr) return false;
	s->SmallGC();
	*res = nullptr;
	return true;
}

bool GCMemSize(SlangInterpreter* s,SlangObj* args,SlangObj** res,Env*){
	if (args!=nullptr) return false;
	size_t size = (s->arena->currPointer-s->arena->currSet)*sizeof(SlangObj);
	*res = s->MakeInt(size);
	return true;
}

bool GCMemCapacity(SlangInterpreter* s,SlangObj* args,SlangObj** res,Env*){
	if (args!=nullptr) return false;
	size_t size = s->arena->memSize/2*sizeof(SlangObj);
	*res = s->MakeInt(size);
	return true;
}

inline bool IsPredefinedSymbol(SymbolName name){
	if (name<GLOBAL_SYMBOL_COUNT){
		return true;
	}
	return false;
}

inline bool GetRecursiveSymbol(Env* e,SymbolName name,SlangObj** val){
	SlangObj** t = nullptr;
	*val = nullptr;
	while (e){
		t = e->GetSymbol(name);
		if (t){
			*val = *t;
			return true;
		}
		e = e->parent;
	}
	return false;
}

inline bool SetRecursiveSymbol(Env* e,SymbolName name,SlangObj* val){
	SlangObj** t = nullptr;
	while (e){
		t = e->GetSymbol(name);
		if (t){
			e->DefSymbol(name,val);
			return true;
		}
		e = e->parent;
	}
	return false;
}

inline bool SlangInterpreter::SlangFuncDefine(SlangObj* argIt,SlangObj** res,Env* env){
	// can't exist
	if (GetType(argIt->left)!=SlangType::Symbol){
		TypeError(argIt->left,GetType(argIt->left),SlangType::Symbol);
		return false;
	}
	if (env->GetSymbol(argIt->left->symbol)){
		EvalError(argIt->left);
		return false;
	}
	
	SymbolName sym = argIt->left->symbol;
	argIt = NextArg(argIt);
	SlangObj* valObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&valObj,env))
		return false;
	
	env->DefSymbol(sym,valObj);
	*res = valObj;
	return true;
}

inline bool SlangInterpreter::SlangFuncLambda(SlangObj* argIt,SlangObj** res,Env* env){
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
		SlangObj* paramIt = argIt->left;
		while (paramIt){
			if (paramIt->type!=SlangType::List){
				TypeError(paramIt,paramIt->type,SlangType::List);
				return false;
			}
			if (GetType(paramIt->left)!=SlangType::Symbol){
				TypeError(paramIt->left,GetType(paramIt->left),SlangType::Symbol);
				return false;
			}
			if (seen.contains(paramIt->left->symbol)){
				std::stringstream msg = {};
				msg << "LambdaError:\n    Lambda parameter '" << 
					*paramIt->left << "' was reused!";
				PushError(paramIt->left,msg.str());
				return false;
			}
			
			params.push_back(paramIt->left->symbol);
			seen.insert(paramIt->left->symbol);
			
			paramIt = paramIt->right;
		}
	} else { // variadic
		params.push_back(argIt->left->symbol);
	}
	argIt = NextArg(argIt);

	SlangObj* lambdaObj = MakeLambda(argIt->left,env,params,variadic);
	*res = lambdaObj;
	return true;
}

inline bool SlangInterpreter::SlangFuncSet(SlangObj* argIt,SlangObj** res,Env* env){
	if (GetType(argIt->left)!=SlangType::Symbol){
		TypeError(argIt->left,GetType(argIt->left),SlangType::Symbol);
		return false;
	}
	
	SymbolName sym = argIt->left->symbol;
	argIt = NextArg(argIt);
	
	SlangObj* valObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&valObj,env))
		return false;
		
	if (!SetRecursiveSymbol(env,sym,valObj)){
		std::stringstream msg = {};
		msg << "SetError:\n    Cannot set undefined symbol '" << *argIt->left << "'";
		PushError(argIt->left,msg.str());
		return false;
	}
	
	*res = valObj;
	return true;
}

inline bool SlangInterpreter::SlangFuncQuote(SlangObj* argIt,SlangObj** res){
	if (argIt->left){
		*res = Copy(argIt->left);
	} else {
		*res = nullptr;
	}
	return true;
}

inline bool ConvertToBool(const SlangObj* obj,bool& res){
	if (!obj){
		res = false;
		return true;
	}
	
	switch (obj->type){
		case SlangType::Bool:
			res = obj->boolean;
			return true;
		case SlangType::Int:
			res = (bool)obj->integer;
			return true;
		case SlangType::Real:
			res = (bool)obj->real;
			return true;
		case SlangType::List:
			res = true;
			return true;
		default:
			assert(0);
			return false;
	}
}

inline bool SlangInterpreter::SlangFuncNot(SlangObj* argIt,SlangObj** res,Env* env){
	SlangObj* boolObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&boolObj,env))
		return false;
	
	bool b;
	if (!ConvertToBool(boolObj,b)){
		TypeError(boolObj,GetType(boolObj),SlangType::Bool);
		return false;
	}
	
	*res = MakeBool(!b);
	return true;
}

inline bool SlangInterpreter::SlangFuncNegate(SlangObj* argIt,SlangObj** res,Env* env){
	SlangObj* numObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&numObj,env))
		return false;
	
	if (!numObj){
		TypeError(numObj,SlangType::NullType,SlangType::Int);
		return false;
	}
	
	switch (numObj->type){
		case SlangType::Int:
			numObj->integer = -numObj->integer;
			*res = numObj;
			return true;
		case SlangType::Real:
			numObj->real = -numObj->real;
			*res = numObj;
			return true;
		default:
			TypeError(numObj,numObj->type,SlangType::Int);
			return false;
	}
}

inline bool SlangInterpreter::SlangFuncPair(SlangObj* argIt,SlangObj** res,Env* env){
	SlangObj* pairObj = AllocateObj();
	// for GC
	StackAddArg(pairObj);
	
	pairObj->type = SlangType::List;
	pairObj->left = nullptr;
	pairObj->right = nullptr;
	
	SlangObj* left = nullptr;
	if (!WrappedEvalExpr(argIt->left,&left,env))
		return false;
		
	funcArgStack[frameIndex-1]->left = left;
		
	argIt = NextArg(argIt);
	
	SlangObj* right = nullptr;
	if (!WrappedEvalExpr(argIt->left,&right,env))
		return false;
	
	funcArgStack[frameIndex-1]->right = right;
	
	*res = funcArgStack[frameIndex-1];
	--frameIndex;
	return true;
}

inline bool SlangInterpreter::SlangFuncLeft(SlangObj* argIt,SlangObj** res,Env* env){
	SlangObj* pairObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&pairObj,env))
		return false;
		
	if (GetType(pairObj)!=SlangType::List){
		TypeError(pairObj,GetType(pairObj),SlangType::List);
		return false;
	}
	
	*res = pairObj->left;
	return true;
}

inline bool SlangInterpreter::SlangFuncRight(SlangObj* argIt,SlangObj** res,Env* env){
	SlangObj* pairObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&pairObj,env))
		return false;
	
	if (GetType(pairObj)!=SlangType::List){
		TypeError(pairObj,GetType(pairObj),SlangType::List);
		return false;
	}
	
	*res = pairObj->right;
	return true;
}

inline bool SlangInterpreter::SlangFuncSetLeft(SlangObj* argIt,SlangObj** res,Env* env){
	SlangObj* pairObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&pairObj,env))
		return false;
		
	if (GetType(pairObj)!=SlangType::List){
		TypeError(pairObj,GetType(pairObj),SlangType::List);
		return false;
	}
	
	argIt = NextArg(argIt);
	
	SlangObj* valObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&valObj,env))
		return false;
	
	pairObj->left = valObj;
	
	*res = pairObj;
	return true;
}

inline bool SlangInterpreter::SlangFuncSetRight(SlangObj* argIt,SlangObj** res,Env* env){
	SlangObj* pairObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&pairObj,env))
		return false;
	
	if (GetType(pairObj)!=SlangType::List){
		TypeError(pairObj,GetType(pairObj),SlangType::List);
		return false;
	}
	
	argIt = NextArg(argIt);
	
	SlangObj* valObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&valObj,env))
		return false;
	
	pairObj->right = valObj;
	*res = pairObj;
	return true;
}

inline bool SlangInterpreter::SlangFuncInc(SlangObj* argIt,SlangObj** res,Env* env){
	SlangObj* numObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&numObj,env))
		return false;
	
	if (GetType(numObj)!=SlangType::Int){
		TypeError(numObj,GetType(numObj),SlangType::Int);
		return false;
	}
	
	*res = MakeInt(numObj->integer+1);
	return true;
}

inline bool SlangInterpreter::SlangFuncDec(SlangObj* argIt,SlangObj** res,Env* env){
	SlangObj* numObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&numObj,env))
		return false;
	
	if (GetType(numObj)!=SlangType::Int){
		TypeError(numObj,GetType(numObj),SlangType::Int);
		return false;
	}
	
	*res = MakeInt(numObj->integer-1);
	return true;
}

inline bool SlangInterpreter::AddReals(SlangObj* argIt,SlangObj** res,Env* env,double curr){
	while (argIt){
		SlangObj* valObj = nullptr;
		if (!WrappedEvalExpr(argIt->left,&valObj,env))
			return false;
		
		if (GetType(valObj)==SlangType::Real){
			curr += valObj->real;
		} else if (GetType(valObj)==SlangType::Int){
			curr += valObj->integer;
		} else {
			TypeError(valObj,GetType(valObj),SlangType::Real);
			return false;
		}
		
		argIt = NextArg(argIt);
	}
	
	*res = MakeReal(curr);
	return true;
}

inline bool SlangInterpreter::SlangFuncAdd(SlangObj* argIt,SlangObj** res,Env* env){
	int64_t sum = 0;
	size_t argCount = 0;
	while (argIt){
		SlangObj* valObj = nullptr;
		if (!WrappedEvalExpr(argIt->left,&valObj,env))
			return false;
		
		if (GetType(valObj)==SlangType::Int){
			sum += valObj->integer;
		} else if (GetType(valObj)==SlangType::Real){
			return AddReals(NextArg(argIt),res,env,valObj->real+(double)sum);
		} else {
			TypeError(valObj,GetType(valObj),SlangType::Int,argCount+1);
			return false;
		}
		
		++argCount;
		argIt = NextArg(argIt);
	}
	
	*res = MakeInt(sum);
	return true;
}

inline bool SlangInterpreter::SlangFuncSub(SlangObj* argIt,SlangObj** res,Env* env){
	SlangObj* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l,env))
		return false;
	
	if (!IsNumeric(l)){
		TypeError(l,GetType(l),SlangType::Int);
		return false;
	}
	StackAddArg(l);
	
	argIt = NextArg(argIt);
	SlangObj* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r,env))
		return false;
		
	if (!IsNumeric(r)){
		TypeError(r,GetType(r),SlangType::Int);
		return false;
	}
	
	l = funcArgStack[frameIndex-1];
	--frameIndex;
	
	if (l->type==SlangType::Int){
		if (r->type==SlangType::Int){
			*res = MakeInt(l->integer - r->integer);
			return true;
		} else {
			*res = MakeReal((double)l->integer - r->real);
			return true;
		}
	} else {
		if (r->type==SlangType::Int){
			*res = MakeReal(l->real - (double)r->integer);
			return true;
		} else {
			*res = MakeReal(l->real - r->real);
			return true;
		}
	}
}

inline bool SlangInterpreter::MulReals(SlangObj* argIt,SlangObj** res,Env* env,double curr){
	while (argIt){
		SlangObj* valObj = nullptr;
		if (!WrappedEvalExpr(argIt->left,&valObj,env))
			return false;
		
		if (GetType(valObj)==SlangType::Real){
			curr *= valObj->real;
		} else if (GetType(valObj)==SlangType::Int){
			curr *= valObj->integer;
		} else {
			TypeError(valObj,GetType(valObj),SlangType::Real);
			return false;
		}
		
		argIt = NextArg(argIt);
	}
	
	*res = MakeReal(curr);
	return true;
}

inline bool SlangInterpreter::SlangFuncMul(SlangObj* argIt,SlangObj** res,Env* env){
	int64_t prod = 1;
	size_t argCount = 0;
	while (argIt){
		SlangObj* valObj = nullptr;
		if (!WrappedEvalExpr(argIt->left,&valObj,env))
			return false;
			
		if (GetType(valObj)==SlangType::Int){
			prod *= valObj->integer;
		} else if (GetType(valObj)==SlangType::Real){
			return MulReals(NextArg(argIt),res,env,(double)prod*valObj->real);
		} else {
			TypeError(valObj,GetType(valObj),SlangType::Int,argCount+1);
			return false;
		}
		
		argIt = NextArg(argIt);
		++argCount;
	}
	
	*res = MakeInt(prod);
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

inline bool SlangInterpreter::SlangFuncDiv(SlangObj* argIt,SlangObj** res,Env* env){
	SlangObj* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l,env))
		return false;
		
	if (!IsNumeric(l)){
		TypeError(l,GetType(l),SlangType::Int);
		return false;
	}
	
	argIt = NextArg(argIt);
	
	SlangObj* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r,env))
		return false;
		
	if (!IsNumeric(r)){
		TypeError(r,GetType(r),SlangType::Int);
		return false;
	}
	
	if (l->type==SlangType::Int){
		if (r->type==SlangType::Int){
			if (r->integer==0){
				ZeroDivisionError(argIt);
				return false;
			}
			*res = MakeInt(floordiv(l->integer,r->integer));
			return true;
		} else {
			if (r->real==0.0){
				ZeroDivisionError(argIt);
				return false;
			}
			*res = MakeReal((double)l->integer/r->real);
			return true;
		}
	} else {
		if (r->type==SlangType::Int){
			if (r->integer==0){
				ZeroDivisionError(argIt);
				return false;
			}
			*res = MakeReal(l->real/(double)r->integer);
			return true;
		} else {
			if (r->real==0.0){
				ZeroDivisionError(argIt);
				return false;
			}
			*res = MakeReal(l->real/r->real);
			return true;
		}
	}
}

inline bool SlangInterpreter::SlangFuncMod(SlangObj* argIt,SlangObj** res,Env* env){
	SlangObj* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l,env))
		return false;
		
	if (!IsNumeric(l)){
		TypeError(l,GetType(l),SlangType::Int);
		return false;
	}
	
	argIt = NextArg(argIt);
	SlangObj* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r,env))
		return false;
		
	if (!IsNumeric(r)){
		TypeError(r,GetType(r),SlangType::Int);
		return false;
	}
	
	if (l->type==SlangType::Int){
		if (r->type==SlangType::Int){
			if (r->integer==0){
				ZeroDivisionError(argIt);
				return false;
			}
			*res = MakeInt(floormod(l->integer,r->integer));
			return true;
		} else {
			if (r->real==0.0){
				ZeroDivisionError(argIt);
				return false;
			}
			*res = MakeReal(fmod((double)l->integer,r->real));
			return true;
		}
	} else {
		if (r->type==SlangType::Int){
			if (r->integer==0){
				ZeroDivisionError(argIt);
				return false;
			}
			*res = MakeReal(fmod(l->real,(double)r->integer));
			return true;
		} else {
			if (r->real==0.0){
				ZeroDivisionError(argIt);
				return false;
			}
			*res = MakeReal(fmod(l->real,r->real));
			return true;
		}
	}
}

inline bool SlangInterpreter::SlangFuncPrint(SlangObj* argIt,SlangObj** res,Env* env){
	SlangObj* printObj = nullptr;
	if (!WrappedEvalExpr(argIt->left,&printObj,env))
		return false;
	
	if (!printObj){
		std::cout << "()\n";
	} else {
		std::cout << *printObj << '\n';
	}
	*res = printObj;
	return true;
}

inline bool SlangInterpreter::SlangFuncAssert(SlangObj* argIt,SlangObj** res,Env* env){
	SlangObj* cond = nullptr;
	if (!WrappedEvalExpr(argIt->left,&cond,env))
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

bool ListEquality(const SlangObj* a,const SlangObj* b);

bool EqualObjs(const SlangObj* a,const SlangObj* b){
	if (a==b) return true;
	if (!a||!b) return false;
	
	if (a->type==b->type){
		if (a->type==SlangType::List){
			return ListEquality(a,b);
		} else {
			// union moment
			return (a->integer==b->integer);
		}
	}
	
	return false;
}

bool ListEquality(const SlangObj* a,const SlangObj* b){
	if (!EqualObjs(a->left,b->left)) return false;
	if (!EqualObjs(a->right,b->right)) return false;
	
	return true;
}

inline bool SlangInterpreter::SlangFuncEq(SlangObj* argIt,SlangObj** res,Env* env){
	SlangObj* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l,env))
		return false;
	StackAddArg(l);
	
	argIt = NextArg(argIt);
	SlangObj* r = nullptr;
	if (!WrappedEvalExpr(argIt->left,&r,env))
		return false;
	
	l = funcArgStack[frameIndex-1];
	--frameIndex;
	
	if (l==r){
		*res = MakeBool(true);
		return true;
	}
	if (!l||!r){
		// one and only one is null
		*res = MakeBool(false);
		return true;
	}
	
	if (l->type==r->type){
		switch (l->type){
			case SlangType::Int:
				*res = MakeBool(l->integer==r->integer);
				return true;
			case SlangType::Real:
				*res = MakeBool(l->real==r->real);
				return true;
			case SlangType::Bool:
				*res = MakeBool(l->boolean==r->boolean);
				return true;
			case SlangType::List:
				*res = MakeBool(ListEquality(l,r));
				return true;
			default:
				return false;
		}
	} else {
		// force the int onto the left
		if (r->type==SlangType::Int){
			std::swap(l,r);
		}
		
		if (l->type==SlangType::Int&&r->type==SlangType::Real){
			*res = MakeBool((double)l->integer==r->real);
			return true;
		}
		
		TypeError(r,r->type,l->type);
		return false;
	}
}

inline bool SlangInterpreter::SlangFuncIs(SlangObj* argIt,SlangObj** res,Env* env){
	SlangObj* l = nullptr;
	if (!WrappedEvalExpr(argIt->left,&l,env))
		return false;
		
	StackAddArg(l);
	
	SlangObj* r = nullptr;
	argIt = NextArg(argIt);
	if (!WrappedEvalExpr(argIt->left,&r,env))
		return false;
		
	l = funcArgStack[frameIndex-1];
	--frameIndex;
	
	if (l==r){
		*res = MakeBool(true);
		return true;
	}
	if (!l||!r){
		*res = MakeBool(false);
		return true;
	}
	
	if (l->type!=r->type){
		*res = MakeBool(false);
		return true;
	}
	
	if (l->type!=SlangType::List){
		if (l->integer==r->integer){
			// union moment
			*res = MakeBool(true);
			return true;
		}
	}
	
	*res = MakeBool(false);
	return true;
}

inline void SlangInterpreter::StackAddArg(SlangObj* arg){
	assert(frameIndex<funcArgStack.size());
	
	gMaxStackHeight = (gMaxStackHeight<frameIndex) ? frameIndex : gMaxStackHeight;
	funcArgStack[frameIndex++] = arg;
}

inline bool SlangInterpreter::WrappedEvalExpr(SlangObj* expr,SlangObj** res,Env* env){
	++gCurrDepth;
	if (gCurrDepth>gMaxDepth) gMaxDepth = gCurrDepth;
	
	bool b = EvalExpr(expr,res,env);
	--gCurrDepth;
	return b;
}

inline size_t GetArgCount(const SlangObj* arg){
	size_t c = 0;
	while (arg){
		arg = NextArg(arg);
		++c;
	}
	return c;
}

inline Env* SlangInterpreter::CreateEnv(){
	Env* env = new Env;
	++gEnvCount;
	++gEnvBalance;
	return env;
}

inline bool SlangInterpreter::GetLetParams(
			std::vector<SymbolName>& params,
			std::vector<SlangObj*>& exprs,
			SlangObj* paramIt){
	while (paramIt){
		SlangObj* pair = paramIt->left;
		if (GetType(pair)!=SlangType::List){
			TypeError(pair,GetType(pair),SlangType::List);
			return false;
		}
		SlangObj* sym = pair->left;
		if (GetType(sym)!=SlangType::Symbol){
			TypeError(sym,GetType(sym),SlangType::Symbol);
			return false;
		}
		params.push_back(sym->symbol);
		pair = pair->right;
		if (GetType(pair)!=SlangType::List){
			TypeError(pair,GetType(pair),SlangType::List);
			return false;
		}
		exprs.push_back(pair->left);
		
		if (pair->right){
			// too many args in init pair
			ArityError(paramIt->left,3,2);
			return false;
		}
		
		paramIt = NextArg(paramIt);
	}
	return true;
}

#define ARITY_EXACT_CHECK(x) \
	if (argCount!=(x)){ \
		ArityError(expr,argCount,(x)); \
		return false; \
	}

bool SlangInterpreter::EvalExpr(SlangObj* expr,SlangObj** res,Env* env){
	++gEvalRecurCounter;
	while (true){
		currEnv = env;
		if (!expr){
			*res = nullptr;
			return true;
		}
		++gEvalCounter;
		switch (expr->type){
			case SlangType::NullType:
			case SlangType::Real:
			case SlangType::Int:
			case SlangType::Bool: {
				*res = expr;
				return true;
			}
			case SlangType::String:
			case SlangType::Lambda: {
				*res = expr;
				return true;
			}
			case SlangType::Symbol: {
				Env* parent = env;
				SlangObj* found = nullptr;
				if (GetRecursiveSymbol(parent,expr->symbol,&found)){
					*res = found;
					return true;
				}
				
				if (IsPredefinedSymbol(expr->symbol)){
					*res = expr;
					return true;
				}
				
				if (extFuncs.contains(expr->symbol)){
					*res = expr;
					return true;
				}
				
				UndefinedError(expr);
				return false;
			}
			
			// function
			case SlangType::List: {
				SlangObj* head = nullptr;
				if (!WrappedEvalExpr(expr->left,&head,env)){
					EvalError(expr);
					return false;
				}
				size_t base = frameIndex;
				
				SlangObj* argIt = NextArg(expr);
				size_t argCount = GetArgCount(argIt);
				
				if (head->type==SlangType::Lambda){
					Env* funcEnv = head->env;
					SlangObj* funcExpr = head->expr;
					if (!funcEnv->variadic&&funcEnv->params.size()!=argCount){
						ArityError(expr,argCount,funcEnv->params.size());
						return false;
					}
					
					if (funcEnv->variadic){
						while (argIt){
							SlangObj* fArg = nullptr;
							if (!WrappedEvalExpr(argIt->left,&fArg,env)){
								EvalError(expr);
								return false;
							}
							StackAddArg(fArg);
							argIt = NextArg(argIt);
						}
						
						SlangObj* list = MakeList(funcArgStack,base,frameIndex);
						funcEnv->DefSymbol(funcEnv->params.front(),list);
					} else {
						for (size_t i=0;i<argCount;++i){
							SlangObj* fArg = nullptr;
							if (!WrappedEvalExpr(argIt->left,&fArg,env)){
								EvalError(expr);
								return false;
							}
							StackAddArg(fArg);
							argIt = NextArg(argIt);
						}
						
						for (size_t i=0;i<argCount;++i){
							funcEnv->DefSymbol(funcEnv->params[i],funcArgStack[base+i]);
						}
					}
					
					frameIndex = base;
					expr = funcExpr;
					env = funcEnv;
					continue;
				} else if (head->type==SlangType::Symbol){
					auto symbol = head->symbol;
					frameIndex = base;
					// handle predefined symbols
					bool success = false;
					switch (symbol){
						case SLANG_DEFINE:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncDefine(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_LAMBDA:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncLambda(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_LET: {
							if (argCount!=2&&argCount!=3){
								ArityError(expr,argCount,2);
								return false;
							}
							
							SymbolName letName;
							// named let
							if (GetType(argIt->left)==SlangType::Symbol){
								letName = argIt->left->symbol;
								argIt = NextArg(argIt);
							} else {
								letName = GenSym();
							}
							
							std::vector<SymbolName> params{};
							std::vector<SlangObj*> initExprs{};
							params.reserve(4);
							initExprs.reserve(4);
							SlangObj* paramIt = argIt->left;
							
							if (!GetLetParams(params,initExprs,paramIt)){
								return false;
							}
							argIt = NextArg(argIt);
							SlangObj* letExpr = argIt->left;
							
							Env* letEnv = CreateEnv();
							letEnv->params = params;
							letEnv->marked = gcParity;
							letEnv->parent = env;
							letEnv->symbolMap = {};
							letEnv->variadic = false;
							// use this to GC the let env
							SlangObj* tempLambda = AllocateObj();
							tempLambda->type = SlangType::Lambda;
							tempLambda->env = letEnv;
							tempLambda->expr = letExpr;
							
							letEnv->DefSymbol(letName,tempLambda);
							
							for (const auto& param : params){
								if (!letEnv->DefSymbol(param,nullptr)){
									RedefinedError(expr,param);
									return false;
								}
							}
							
							size_t i=0;
							for (const auto& param : params){
								SlangObj* initObj;
								if (!WrappedEvalExpr(initExprs[i],&initObj,letEnv)){
									EvalError(initExprs[i]);
									return false;
								}
								letEnv->DefSymbol(param,initObj);
								++i;
							}
							
							// last call tail call
							expr = letExpr;
							env = letEnv;
							continue;
						}
						case SLANG_SET:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncSet(argIt,res,env);
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
								if (!WrappedEvalExpr(argIt->left,res,env)){
									EvalError(argIt->left);
									return false;
								}
								argIt = NextArg(argIt);
							}
							
							// elide last call
							expr = argIt->left;
							continue;
						}
						case SLANG_IF: {
							ARITY_EXACT_CHECK(3);
							bool test;
							SlangObj* condObj = nullptr;
							if (!WrappedEvalExpr(argIt->left,&condObj,env)){
								EvalError(expr);
								return false;
							}
							if (!ConvertToBool(condObj,test)){
								TypeError(argIt->left,GetType(argIt->left),SlangType::Bool);
								EvalError(expr);
								return false;
							}
							
							argIt = NextArg(argIt);
							if (test){
								expr = argIt->left;
								continue;
							}
							argIt = NextArg(argIt);
							expr = argIt->left;
							continue;
						}
						case SLANG_QUOTE:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncQuote(argIt,res);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_NOT:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncNot(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_NEG:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncNegate(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_PAIR:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncPair(argIt,res,env);
							return success;
						case SLANG_LEFT:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncLeft(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_RIGHT:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncRight(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_INC:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncInc(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_DEC:	
							ARITY_EXACT_CHECK(1);
							success = SlangFuncDec(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_ADD:
							success = SlangFuncAdd(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_SUB:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncSub(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_MUL:
							success = SlangFuncMul(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_DIV:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncDiv(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_MOD:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncMod(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_SET_LEFT:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncSetLeft(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_SET_RIGHT:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncSetRight(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_PRINT:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncPrint(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_ASSERT:
							ARITY_EXACT_CHECK(1);
							success = SlangFuncAssert(argIt,res,env);
							return success;
						case SLANG_EQ:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncEq(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_IS:
							ARITY_EXACT_CHECK(2);
							success = SlangFuncIs(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						default:
							if (!extFuncs.contains(symbol)){
								ProcError(head);
								return false;
							}
							success = extFuncs.at(symbol)(this,argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
					}
				} else {
					TypeError(head,GetType(head),SlangType::Lambda);
					return false;
				}
			}
		}
	}
}

void SlangInterpreter::PushError(const SlangObj* expr,const std::string& msg){
	LocationData loc = parser.GetExprLocation(expr);
	errors.emplace_back(loc,msg);
}

void SlangInterpreter::EvalError(const SlangObj* expr){
	if (expr==program) return;
	std::stringstream msg = {};
	msg << "EvalError:\n    Error while evaluating '" << *expr << "'";
	PushError(expr,msg.str());
}

void SlangInterpreter::UndefinedError(const SlangObj* expr){
	std::stringstream msg = {};
	msg << "UndefinedError:\n    Symbol '" << *expr << "' is not defined!";
	PushError(expr,msg.str());
}

void SlangInterpreter::RedefinedError(const SlangObj* expr,SymbolName sym){
	std::stringstream msg = {};
	msg << "RedefinedError:\n    Symbol '" << 
		parser.GetSymbolString(sym) << "' in expr '" << *expr << "' was redefined!";
	PushError(expr,msg.str());
}

inline const char* TypeToString(SlangType type){
	switch (type){
		case SlangType::Int:
			return "Int";
		case SlangType::Real:
			return "Real";
		case SlangType::String:
			return "String";
		case SlangType::List:
			return "Pair";
		case SlangType::Bool:
			return "Bool";
		case SlangType::Symbol:
			return "Symbol";
		case SlangType::Lambda:
			return "Lambda";
		case SlangType::NullType:
			return "NullType";
	}
	
	static char errArr[32];
	sprintf(errArr,"Invalid %d",(int)type);
	return errArr;
}

void SlangInterpreter::TypeError(const SlangObj* expr,SlangType found,SlangType expect,size_t index){
	std::stringstream msg = {};
	msg << "TypeError:\n    Expected type " << 
		TypeToString(expect) << " instead of type " << 
		TypeToString(found) << " in argument '" << *expr << "'";
	if (index!=-1ULL){
		msg << " (arg #" << index << ")";
	}
		
	PushError(expr,msg.str());
}

void SlangInterpreter::AssertError(const SlangObj* expr){
	std::stringstream msg = {};
	msg << "AssertError:\n    Assertion failed: '" << *expr << "'";
	
	PushError(expr,msg.str());
}

void SlangInterpreter::ProcError(const SlangObj* proc){
	std::stringstream msg = {};
	msg << "ProcError:\n    Procedure '" << *proc << "' is not defined!";
	
	PushError(proc,msg.str());
}

void SlangInterpreter::ArityError(const SlangObj* head,size_t found,size_t expect){
	std::stringstream msg = {};
	const char* plural = (expect!=1) ? "s" : "";
	
	msg << "ArityError:\n    Procedure '" << *head << 
		"' takes " << expect << " argument" << plural << ", was given " <<
		found;
		
	PushError(head,msg.str());
}

void SlangInterpreter::ZeroDivisionError(const SlangObj* head){
	std::stringstream msg = {};
	
	msg << "ZeroDivisionError:\n    Division by zero";
		
	PushError(head,msg.str());
}

void SlangInterpreter::Run(SlangObj* prog){
	arena = new MemArena();
	size_t memSize = SMALL_SET_SIZE*2;
	SlangObj* memAlloc = (SlangObj*)malloc(memSize*sizeof(SlangObj));
	arena->SetSpace(memAlloc,memSize);
	memset(arena->memSet,0,memSize*sizeof(SlangObj));
	gDebugInterpreter = this;
	
	funcArgStack.resize(32);
	frameIndex = 0;
	baseIndex = 0;
	errors = {};
	genIndex = (1ULL<<63);
	
	program = prog;
	SlangObj* res = nullptr;
	
	auto start = std::chrono::steady_clock::now();
	WrappedEvalExpr(prog,&res,&env);
	std::chrono::duration<double> diff = std::chrono::steady_clock::now()-start;
	gRunTime = diff.count();
	
	if (!errors.empty()){
		PrintErrors(errors);
		std::cout << '\n';
	}
	
	currEnv = nullptr;
	env.symbolMap.clear();
	frameIndex = 0;
	SmallGC();
	
	size_t tenureCount = arena->tenureSets.size();
	arena->ClearTenuredSets();
	
	std::cout << "Alloc balance: " << gAllocCount << '\n';
	std::cout << "Alloc total: " << gAllocTotal << " (" << gAllocTotal*sizeof(SlangObj)/1024 << " KB)\n";
	std::cout << "Tenure count: " << gTenureCount << " (" << gTenureCount*sizeof(SlangObj)/1024 << " KB)\n";
	std::cout << "Tenure set count: " << tenureCount << '\n';
	std::cout << "Heap alloc total: " << gHeapAllocTotal << " (" << gHeapAllocTotal*sizeof(SlangObj)/1024 << " KB)\n";
	std::cout << "Eval count: " << gEvalCounter << '\n';
	std::cout << "Non-eliminated eval count: " << gEvalRecurCounter << '\n';
	std::cout << "Max stack height: " << gMaxStackHeight << '\n';
	std::cout << "Small GCs: " << gSmallGCs << '\n';
	std::cout << "Realloc count: " << gReallocCount << '\n';
	std::cout << "Max arena size: " << gMaxArenaSize/1024 << " KB\n";
	std::cout << "Curr arena size: " << arena->memSize*sizeof(SlangObj)/1024 << " KB\n";
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
	} else if (c==';'){
		token.type = SlangTokenType::Comment;
		while (pos!=end&&*pos!='\n') ++pos;
	} else if ((c=='-'&&((nextC>='0'&&nextC<='9')||nextC=='.'))||(c>='0'&&c<='9')||c=='.'){
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
	if (token.view=="true"){
		token.type = SlangTokenType::True;
	} else if (token.view=="false"){
		token.type = SlangTokenType::False;
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

inline SlangObj* HeapAllocObj(){
	++gHeapAllocTotal;
	SlangObj* o = new SlangObj;
	o->flags = 0;
	return o;
}

inline SlangObj* WrapExprIn(SymbolName func,SlangObj* expr){
	SlangObj* q = HeapAllocObj();
	q->type = SlangType::Symbol;
	q->symbol = func;

	SlangObj* list = HeapAllocObj();
	list->type = SlangType::List;
	list->left = q;
	list->right = HeapAllocObj();
	list->right->type = SlangType::List;
	list->right->left = expr;
	list->right->right = nullptr;

	return list;
}

inline SlangObj* WrapExprSplice(SymbolName func,SlangObj* expr){
	SlangObj* q = HeapAllocObj();
	q->type = SlangType::Symbol;
	q->symbol = func;

	SlangObj* list = HeapAllocObj();
	list->type = SlangType::List;
	list->left = q;
	list->right = expr;

	return list;
}

inline bool SlangParser::ParseObj(SlangObj** res){
	LocationData loc = {token.line,token.col,0};
	switch (token.type){
		case SlangTokenType::Int: {
			char* d;
			int64_t i = strtoll(token.view.begin(),&d,10);
			NextToken();
			SlangObj* intObj = HeapAllocObj();
			intObj->type = SlangType::Int;
			intObj->integer = i;
			codeMap[intObj] = loc;
			*res = intObj;
			return true;
		}
		case SlangTokenType::Real: {
			double r = std::stod(token.view.begin(),nullptr);
			NextToken();
			SlangObj* realObj = HeapAllocObj();
			realObj->type = SlangType::Real;
			realObj->real = r;
			codeMap[realObj] = loc;
			*res = realObj;
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
			SlangObj* sym = HeapAllocObj();
			sym->type = SlangType::Symbol;
			sym->symbol = s;
			codeMap[sym] = loc;
			*res = sym;
			return true;
		}
		case SlangTokenType::True: {
			NextToken();
			SlangObj* boolObj = HeapAllocObj();
			boolObj->type = SlangType::Bool;
			boolObj->boolean = true;
			codeMap[boolObj] = loc;
			*res = boolObj;
			return true;
		}
		case SlangTokenType::False: {
			NextToken();
			SlangObj* boolObj = HeapAllocObj();
			boolObj->type = SlangType::Bool;
			boolObj->boolean = false;
			codeMap[boolObj] = loc;
			*res = boolObj;
			return true;
		}
		case SlangTokenType::Quote: {
			NextToken();
			SlangObj* sub;
			if (!ParseObj(&sub)) return false;
			*res = WrapExprIn(SLANG_QUOTE,sub);
			codeMap[*res] = loc;
			return true;
		}
		case SlangTokenType::Not: {
			NextToken();
			SlangObj* sub;
			if (!ParseObj(&sub)) return false;
			*res = WrapExprIn(SLANG_NOT,sub);
			codeMap[*res] = loc;
			return true;
		}
		case SlangTokenType::Negation: {
			NextToken();
			SlangObj* sub;
			if (!ParseObj(&sub)) return false;
			*res = WrapExprIn(SLANG_NEG,sub);
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
void SlangParser::TestSeqMacro(SlangObj* list,size_t argCount){
	if (GetType(list->left)!=SlangType::Symbol){
		return;
	}
	
	switch (list->left->symbol){
		case SLANG_LET: {
			if (argCount<=3) return;
			// is named?
			if (GetType(list->right->left)==SlangType::Symbol){
				if (argCount<=4) return;
				SlangObj* doWrapper = WrapExprSplice(SLANG_DO,list->right->right->right);
				SlangObj* newHead = HeapAllocObj();
				newHead->type = SlangType::List;
				newHead->left = doWrapper;
				newHead->right = nullptr;
				list->right->right->right = newHead;
			} else {
				// (let (()) b1 b2...)
				SlangObj* doWrapper = WrapExprSplice(SLANG_DO,list->right->right);
				SlangObj* newHead = HeapAllocObj();
				newHead->type = SlangType::List;
				newHead->left = doWrapper;
				newHead->right = nullptr;
				list->right->right = newHead;
			}
			return;
		}
		case SLANG_LAMBDA: {
			if (argCount<=3) return;
			SlangObj* doWrapper = WrapExprSplice(SLANG_DO,list->right->right);
			SlangObj* newHead = HeapAllocObj();
			newHead->type = SlangType::List;
			newHead->left = doWrapper;
			newHead->right = nullptr;
			list->right->right = newHead;
			return;
		}
		default:
			break;
	}
}

bool SlangParser::ParseExpr(SlangObj** res){
	*res = nullptr;
	
	SlangObj* obj = nullptr;
	size_t argCount = 0;
	char leftBracket = token.view[0];
	NextToken();
	
	while (token.type!=SlangTokenType::RightBracket){
		if (!obj){
			obj = HeapAllocObj();
			*res = obj;
		} else {
			obj->right = HeapAllocObj();
			obj = obj->right;
		}
		obj->type = SlangType::List;
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
		TestSeqMacro(*res,argCount);
	}
	
	NextToken();
	return true;
}

// wraps program exprs in a (do ...) block
inline SlangObj* WrapProgram(const std::vector<SlangObj*>& exprs){
	SlangObj* doSym = HeapAllocObj();
	doSym->type = SlangType::Symbol;
	doSym->symbol = SLANG_DO;

	SlangObj* outer = HeapAllocObj();
	outer->type = SlangType::List;
	outer->left = doSym;
	outer->right = nullptr;
	
	if (exprs.empty()) return outer;
	
	SlangObj* next = nullptr;
	for (auto* expr : exprs){
		if (!next){
			next = HeapAllocObj();
			outer->right = next;
		} else {
			next->right = HeapAllocObj();
			next = next->right;
		}
		next->type = SlangType::List;
		next->left = expr;
		next->right = nullptr;
	}
	
	return outer;
}

SlangObj* SlangParser::LoadIntoBuffer(SlangObj* prog){
	size_t i = codeIndex++;
	codeBuffer[i] = *prog;
	
	if (prog->type==SlangType::List){
		prog->left = LoadIntoBuffer(prog->left);
		prog->right = LoadIntoBuffer(prog->right);
	}
	
	return &codeBuffer[i];
}

SlangObj* SlangParser::Parse(const std::string& code){
	gDebugParser = this;
	
	codeBuffer = {};
	codeIndex = 0;
	size_t codeStart = gHeapAllocTotal;
	
	auto start = std::chrono::steady_clock::now();
	
	tokenizer.SetText(code);
	token.type = SlangTokenType::Comment;
	NextToken();
	
	std::vector<SlangObj*> exprs = {};
	SlangObj* temp;
	while (errors.empty()){
		if (!ParseObj(&temp)) break;
		exprs.push_back(temp);
		
		if (token.type==SlangTokenType::EndOfFile) break;
	}
	
	if (!errors.empty()){
		PrintErrors(errors);
		return nullptr;
	}
	
	SlangObj* prog = WrapProgram(exprs);
	
	std::chrono::duration<double> diff = std::chrono::steady_clock::now()-start;
	gParseTime = diff.count();
	
	codeCount = gHeapAllocTotal - codeStart;
	codeBuffer.resize(codeCount);
	//LoadIntoBuffer(prog);
	
	return prog;
}

inline LocationData SlangParser::GetExprLocation(const SlangObj* expr){
	if (!codeMap.contains(expr))
		return {-1U,-1U,-1U};
	
	return codeMap.at(expr);
}

SlangParser::SlangParser() :
		tokenizer(),token(),
		nameDict(gDefaultNameDict),
		currentName(GLOBAL_SYMBOL_COUNT),errors(){
	token.type = SlangTokenType::Comment;
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

SlangObj* SlangInterpreter::Parse(const std::string& code){
	return parser.Parse(code);
}

std::ostream& operator<<(std::ostream& os,SlangObj obj){
	switch (obj.type){
		case SlangType::NullType:
			break;
		case SlangType::Int:
			os << obj.integer;
			break;
		case SlangType::Real:
			os << obj.real;
			break;
		case SlangType::Lambda: {
			os << "(& ";
			if (!obj.env->variadic)
				os << "(";
				
			for (size_t i=0;i<obj.env->params.size();++i){
				if (i!=0)
					os << ' ';
				os << gDebugParser->GetSymbolString(obj.env->params[i]);
			}
			if (!obj.env->variadic)
				os << ")";
			os << " ";
			os << *obj.expr << ")";
			break;
		}
		case SlangType::Symbol:
			os << gDebugParser->GetSymbolString(obj.symbol);
			break;
		case SlangType::String:
			os << obj.character;
			break;
		case SlangType::Bool:
			if (obj.boolean){
				os << "true";
			} else {
				os << "false";
			}
			break;
		case SlangType::List: {
			os << '(';
			if (!obj.left){
				os << "()";
			} else {
				os << *obj.left;
			}
			SlangObj* next = obj.right;
			while (next){
				os << ' ';
				if(next->type!=SlangType::List){
					os << ". " << *next;
					break;
				}
				if (!next->left){
					os << "()";
				} else {
					os << *next->left;
				}
				next = next->right;
			}
			os << ')';
			break;
		}
	}
	
	return os;
}

}
