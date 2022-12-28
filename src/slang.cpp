#include "slang.h"

#include <assert.h>
#include <iostream>
#include <cstring>
#include <sstream>

#include <set>

namespace slang {

MemArena gMemArena = {};

size_t gAllocCount = 0;
size_t gAllocTotal = 0;
size_t gMaxAlloc = 0;
size_t gHeapAllocTotal = 0;
size_t gMaxStackHeight = 0;
std::set<SlangObj*> stillAround = {};

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
	SlangObj* p = (SlangObj*)arena->smallPointer;
	arena->smallPointer += sizeof(SlangObj);
	if (arena->smallPointer>=arena->smallSetEnd)
		arena->smallPointer = &arena->smallSet[0];
	//SlangObj* p = (SlangObj*)malloc(sizeof(SlangObj));
	++gAllocCount;
	++gAllocTotal;
	gMaxAlloc = (gAllocCount>gMaxAlloc) ? gAllocCount : gMaxAlloc;
	//stillAround.insert(p);
	p->flags = 0;
	return p;
}

inline void SlangInterpreter::FreeObj(SlangObj* p){
	/*if (!stillAround.contains(p)){
		std::cout << "double free!\n";
	} else {
		stillAround.erase(p);
	}*/
	//free(p);
	p->flags |= FLAG_FREE;
	--gAllocCount;
}

void SlangInterpreter::FreeExpr(SlangObj* p){
	if (p->type==SlangType::List){
		FreeExpr(p->left);
		FreeExpr(p->right);
	}
	FreeObj(p);
}

void SlangInterpreter::FreeEnv(Env* e){
	for (auto* child : e->children){
		FreeEnv(child);
		delete child;
	}
	
	for (auto [key, val] : e->symbolMap){
		FreeExpr(val);
	}
}

SlangObj* SlangInterpreter::Copy(const SlangObj* expr){
	SlangObj* n = AllocateObj();
	memcpy(n,expr,sizeof(SlangObj));
	if (expr->type==SlangType::List){
		n->left = Copy(expr->left);
		n->right = Copy(expr->right);
	}
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
		   c=='~';
}

enum SlangGlobalSymbol {
	SLANG_DEFINE = 0,
	SLANG_LAMBDA,
	SLANG_SET,
	SLANG_IF,
	SLANG_DO,
	SLANG_QUOTE,
	SLANG_PAIR,
	SLANG_LEFT,
	SLANG_RIGHT,
	SLANG_SET_LEFT,
	SLANG_SET_RIGHT,
	SLANG_ADD,
	SLANG_SUB,
	SLANG_MUL,
	SLANG_DIV,
	SLANG_PRINT,
	GLOBAL_SYMBOL_COUNT
};

SymbolNameDict gDefaultNameDict = {
	{"def",SLANG_DEFINE},
	{"&",SLANG_LAMBDA},
	{"set!",SLANG_SET},
	{"if",SLANG_IF},
	{"do",SLANG_DO},
	{"quote",SLANG_QUOTE},
	{"pair",SLANG_PAIR},
	{"L",SLANG_LEFT},
	{"R",SLANG_RIGHT},
	{"setL!",SLANG_SET_LEFT},
	{"setR!",SLANG_SET_RIGHT},
	{"+",SLANG_ADD},
	{"-",SLANG_SUB},
	{"*",SLANG_MUL},
	{"/",SLANG_DIV},
	{"print",SLANG_PRINT}
};

void Env::DefSymbol(SymbolName name,SlangObj* obj){
	symbolMap.insert_or_assign(name,obj);
}

SlangObj* Env::GetSymbol(SymbolName name) const {
	if (!symbolMap.contains(name))
		return nullptr;
		
	return symbolMap.at(name);
}
inline Env* SlangInterpreter::MakeEnvChild(Env* parent){
	Env* child = new Env;
	*child = {{},{},parent};
	if (parent->parent){
		for (const auto& [name,sym] : parent->symbolMap){
			child->DefSymbol(name,Copy(sym));
		}
	}
	parent->children.push_back(child);
	return child;
}
/*
inline void Env::Clear(){
	for (const auto& [name,sym] : symbolMap){
		FreeExpr(sym);
	}
	symbolMap.clear();
}*/

inline SlangObj* SlangInterpreter::MakeInt(int64_t i){
	SlangObj* obj = AllocateObj();
	obj->type = SlangType::Int;
	obj->integer = i;
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

inline SlangObj* SlangInterpreter::MakeList(const std::vector<SlangObj*>& objs){
	SlangObj* listHead = AllocateObj();
	listHead->type = SlangType::List;
	SlangObj* list = listHead;
	for (auto* obj : objs){
		list->left = obj;
		list->right = AllocateObj();
		list = list->right;
		list->type = SlangType::List;
	}
	list->type = SlangType::Null;
	return listHead;
}

inline bool GetRecursiveSymbol(Env* e,SymbolName name,SlangObj** val){
	while (e){
		*val = e->GetSymbol(name);
		if (*val){
			return true;
		}
		e = e->parent;
	}
	return false;
}

inline bool SetRecursiveSymbol(SlangInterpreter* alloc,Env* e,SymbolName name,SlangObj* val){
	while (e){
		SlangObj* res = e->GetSymbol(name);
		if (res){
			e->DefSymbol(name,alloc->Copy(val));
			return true;
		}
		e = e->parent;
	}
	return false;
}

inline SlangObj* NextArg(const SlangObj* expr){
	return expr->right;
}

inline bool SlangInterpreter::SlangFuncDefine(SlangObj* argIt,SlangObj* res,Env* env){
	// can't exist
	//assert(env->GetSymbol(args[0]->symbol)==nullptr);
	if (argIt->left->type!=SlangType::Symbol){
		TypeError(argIt->left,argIt->left->type,SlangType::Symbol);
		return false;
	}
		
	SymbolName sym = argIt->left->symbol;
	argIt = NextArg(argIt);
	if (!EvalExpr(argIt->left,res,env))
		return false;
	
	SlangObj* h = AllocateObj();
	*h = *res;
	env->DefSymbol(sym,h);
	return true;
}

inline bool SlangInterpreter::SlangFuncLambda(SlangObj* argIt,SlangObj* res,Env* env){
	if (argIt->left->type!=SlangType::List&&argIt->left->type!=SlangType::Null){
		std::stringstream msg = {};
		msg << "LambdaError:\n    Expected parameter list, not '" << *argIt->left << "'";
		PushError(argIt->left,msg.str());
		return false;
	}
		
	std::vector<SymbolName> params = {};
	std::set<SymbolName> seen = {};
	SlangObj* paramIt = argIt->left;
	while (paramIt->type!=SlangType::Null){
		if (paramIt->type!=SlangType::List){
			TypeError(paramIt,paramIt->type,SlangType::List);
			return false;
		}
		if (paramIt->left->type!=SlangType::Symbol){
			TypeError(paramIt->left,paramIt->left->type,SlangType::Symbol);
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

	Env* child = MakeEnvChild(env);
	argIt = NextArg(argIt);
	SlangLambda lam = {params,argIt->left,child};
	LambdaKey key = RegisterLambda(lam);
	res->type = SlangType::Lambda;
	res->lambda = key;
	return true;
}

inline bool SlangInterpreter::SlangFuncSet(SlangObj* argIt,SlangObj* res,Env* env){
	if (argIt->left->type!=SlangType::Symbol){
		TypeError(argIt->left,argIt->left->type,SlangType::Symbol);
		return false;
	}
	SymbolName sym = argIt->left->symbol;
	argIt = NextArg(argIt);
	
	if (!EvalExpr(argIt->left,res,env))
		return false;
		
	if (!SetRecursiveSymbol(this,env,sym,res)){
		std::stringstream msg = {};
		msg << "SetError:\n    Cannot set undefined symbol '" << *argIt->left << "'";
		PushError(argIt->left,msg.str());
		return false;
	}
	
	return true;
}

inline bool SlangInterpreter::SlangFuncQuote(SlangObj* argIt,SlangObj* res){
	//if (argIt->type==SlangType::Null)
	//	return false;
	
	*res = *argIt->left;
	return true;
}

inline bool SlangInterpreter::SlangFuncPair(SlangObj* argIt,SlangObj* res,Env* env){
	res->type = SlangType::List;
	res->left = AllocateObj();
	res->right = AllocateObj();
	
	if (!WrappedEvalExpr(argIt->left,res->left,env))
		return false;
		
	argIt = NextArg(argIt);
	
	if (!WrappedEvalExpr(argIt->left,res->right,env))
		return false;
	
	return true;
}

inline bool SlangInterpreter::SlangFuncLeft(SlangObj* argIt,SlangObj* res,Env* env){
	if (!WrappedEvalExpr(argIt->left,res,env))
		return false;
		
	if (res->type!=SlangType::List){
		TypeError(res,res->type,SlangType::List);
		return false;
	}
	
	*res = *res->left;
	return true;
}

inline bool SlangInterpreter::SlangFuncRight(SlangObj* argIt,SlangObj* res,Env* env){
	if (!WrappedEvalExpr(argIt->left,res,env))
		return false;
	
	if (res->type!=SlangType::List){
		TypeError(res,res->type,SlangType::List);
		return false;
	}
		
	*res = *res->right;
	return true;
}

inline bool SlangInterpreter::SlangFuncSetLeft(SlangObj* argIt,SlangObj* res,Env* env){
	// TODO: make res a SlangObj**
	SlangObj* toSet = StackAlloc();
	if (!WrappedEvalExpr(argIt->left,toSet,env))
		return false;
		
	if (toSet->type!=SlangType::List){
		TypeError(toSet,toSet->type,SlangType::List);
		return false;
	}
	
	argIt = NextArg(argIt);
	
	SlangObj* val = StackAlloc();
	if (!WrappedEvalExpr(argIt->left,val,env))
		return false;
	
	*toSet->left = *val;
	*res = *val;
	return true;
}

inline bool SlangInterpreter::SlangFuncSetRight(SlangObj* argIt,SlangObj* res,Env* env){
	SlangObj* toSet = StackAlloc();
	if (!WrappedEvalExpr(argIt->left,toSet,env))
		return false;
	
	if (toSet->type!=SlangType::List){
		TypeError(toSet,toSet->type,SlangType::List);
		return false;
	}
	
	argIt = NextArg(argIt);
	
	SlangObj* val = StackAlloc();
	if (!WrappedEvalExpr(argIt->left,val,env))
		return false;
	
	*toSet->right = *val;
	*res = *val;
	return true;
}

inline bool SlangInterpreter::SlangFuncAdd(SlangObj* argIt,SlangObj* res,Env* env){
	int64_t sum = 0;
	while (argIt->type!=SlangType::Null){
		if (!EvalExpr(argIt->left,res,env))
			return false;
			
		if (res->type!=SlangType::Int){
			TypeError(res,res->type,SlangType::Int);
			return false;
		}
			
		sum += res->integer;
		argIt = NextArg(argIt);
	}
	res->type = SlangType::Int;
	res->integer = sum;
	return true;
}

inline bool SlangInterpreter::SlangFuncSub(SlangObj* argIt,SlangObj* res,Env* env){
	if (!EvalExpr(argIt->left,res,env))
		return false;
	if (res->type!=SlangType::Int){
		TypeError(res,res->type,SlangType::Int);
		return false;
	}
	
	argIt = NextArg(argIt);
	int64_t diff = res->integer;
	while (argIt->type!=SlangType::Null){
		if (!EvalExpr(argIt->left,res,env))
			return false;
		
		if (res->type!=SlangType::Int){
			TypeError(res,res->type,SlangType::Int);
			return false;
		}
			
		diff -= res->integer;
		argIt = NextArg(argIt);
	}
	res->type = SlangType::Int;
	res->integer = diff;
	return true;
}

inline bool SlangInterpreter::SlangFuncMul(SlangObj* argIt,SlangObj* res,Env* env){
	int64_t prod = 1;
	while (argIt->type!=SlangType::Null){
		if (!EvalExpr(argIt->left,res,env))
			return false;
			
		if (res->type!=SlangType::Int){
			TypeError(res,res->type,SlangType::Int);
			return false;
		}
			
		prod *= res->integer;
		argIt = NextArg(argIt);
	}
	res->type = SlangType::Int;
	res->integer = prod;
	return true;
}

inline bool SlangInterpreter::SlangFuncDiv(SlangObj* argIt,SlangObj* res,Env* env){
	if (!EvalExpr(argIt->left,res,env))
		return false;
	if (res->type!=SlangType::Int){
		TypeError(res,res->type,SlangType::Int);
		return false;
	}
	
	argIt = NextArg(argIt);
	int64_t quo = res->integer;
	while (argIt->type!=SlangType::Null){
		if (!EvalExpr(argIt->left,res,env))
			return false;
		
		if (res->type!=SlangType::Int){
			TypeError(res,res->type,SlangType::Int);
			return false;
		}
			
		quo /= res->integer;
		argIt = NextArg(argIt);
	}
	res->type = SlangType::Int;
	res->integer = quo;
	return true;
}

inline bool SlangInterpreter::SlangFuncPrint(SlangObj* argIt,SlangObj* res,Env* env){
	if (!EvalExpr(argIt->left,res,env))
		return false;
	
	std::cout << *res << '\n';
	return true;
}

bool SlangInterpreter::Validate(SlangObj* expr){
	switch (expr->type){
		case SlangType::Int:
		case SlangType::Real:
		case SlangType::Null:
		case SlangType::String:
		case SlangType::Bool:
		case SlangType::Lambda:
		case SlangType::Symbol:
			return true;
		case SlangType::List: {
			
			
			return true;
		}
	}
	return false;
}

inline bool SlangInterpreter::WrappedEvalExpr(SlangObj* expr,SlangObj* res,Env* env){
	StackAllocFrame();
	bool b = EvalExpr(expr,res,env);
	StackFreeFrame();
	return b;
}

inline size_t GetArgCount(const SlangObj* arg){
	size_t c = 0;
	while (arg->type!=SlangType::Null){
		arg = NextArg(arg);
		++c;
	}
	return c;
}

bool SlangInterpreter::EvalExpr(SlangObj* expr,SlangObj* res,Env* env){
	++gEvalRecurCounter;
	while (true){
		++gEvalCounter;
		switch (expr->type){
			case SlangType::Real:
			case SlangType::Null:
			case SlangType::Int:
			case SlangType::String:
			case SlangType::Bool:
			case SlangType::Lambda: {
				*res = *expr;
				return true;
			}
			case SlangType::Symbol: {
				Env* parent = env;
				SlangObj* found;
				if (GetRecursiveSymbol(parent,expr->symbol,&found)){
					*res = *found;
					return true;
				}
				
				*res = *expr;
				return true;
			}
			
			// function
			case SlangType::List: {
				size_t resetPoint = frameIndex;
				SlangObj head = {};
				if (!EvalExpr(expr->left,&head,env)){
					EvalError(expr);
					return false;
				}
				frameIndex = resetPoint;
				
				SlangObj* argIt = NextArg(expr);
				size_t argCount = GetArgCount(argIt);
				
				if (head.type==SlangType::Lambda){
					SlangLambda* lam = GetLambda(head.lambda);
					
					if (lam->params.size()!=argCount){
						ArityError(expr,argCount,lam->params.size());
						return false;
					}
					
					size_t tempBase = frameIndex;
					for (size_t i=0;i<argCount;++i){
						if (!WrappedEvalExpr(argIt->left,StackAlloc(),env)){
							EvalError(expr);
							return false;
						}
						argIt = NextArg(argIt);
					}
					if (argIt->type!=SlangType::Null){
						EvalError(expr);
						return false;
					}
					
					Env* funcEnv = lam->env;
					if (funcEnv!=env){
						for (size_t i=0;i<argCount;++i){
							funcEnv->DefSymbol(lam->params[i],&stack[tempBase++]);
						}
						assert(tempBase==frameIndex);
					} else { // tail call stack eliasion
						for (size_t i=0;i<argCount;++i){
							auto* sym = funcEnv->GetSymbol(lam->params[i]);
							*sym = stack[tempBase++];
						}
						assert(tempBase==frameIndex);
						frameIndex = resetPoint;
					}
					
					expr = lam->body;
					env = funcEnv;
					continue;
				} else if (head.type==SlangType::Symbol){
					auto symbol = head.symbol;
					// handle predefined symbols
					bool success = false;
					switch (symbol){
						case SLANG_DEFINE:
							if (argCount!=2){
								ArityError(expr,argCount,2);
								return false;
							}
							success = SlangFuncDefine(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_LAMBDA:
							if (argCount!=2){
								ArityError(expr,argCount,2);
								return false;
							}
							success = SlangFuncLambda(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_SET:
							if (argCount!=2){
								ArityError(expr,argCount,2);
								return false;
							}
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
							while (argIt->right->type!=SlangType::Null){
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
							if (argCount!=3){
								ArityError(expr,argCount,3);
								return false;
							}
							bool test;
							
							if (!WrappedEvalExpr(argIt->left,res,env)){
								EvalError(expr);
								return false;
							}
								
							if (res->type==SlangType::Int){
								test = (bool)res->integer;
							} else if (res->type==SlangType::Bool){
								test = res->boolean;
							} else {
								TypeError(argIt->left,argIt->type,SlangType::Bool);
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
							if (argCount!=1){
								ArityError(expr,argCount,1);
								return false;
							}
							success = SlangFuncQuote(argIt,res);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_PAIR:
							if (argCount!=2){
								ArityError(expr,argCount,2);
								return false;
							}
							success = SlangFuncPair(argIt,res,env);
							return success;
						case SLANG_LEFT:
							if (argCount!=1){
								ArityError(expr,argCount,1);
								return false;
							}
							success = SlangFuncLeft(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_RIGHT:
							if (argCount!=1){
								ArityError(expr,argCount,1);
								return false;
							}
							success = SlangFuncRight(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_ADD:
							success = SlangFuncAdd(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_SUB:
							if (argCount<2){
								ArityError(expr,argCount,2);
								return false;
							}
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
							if (argCount<2){
								ArityError(expr,argCount,2);
								return false;
							}
							success = SlangFuncDiv(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_SET_LEFT:
							if (argCount!=2){
								ArityError(expr,argCount,2);
								return false;
							}
							success = SlangFuncSetLeft(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_SET_RIGHT:
							if (argCount!=2){
								ArityError(expr,argCount,2);
								return false;
							}
							success = SlangFuncSetRight(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						case SLANG_PRINT:
							if (argCount!=1){
								ArityError(expr,argCount,1);
								return false;
							}
							success = SlangFuncPrint(argIt,res,env);
							if (!success)
								EvalError(expr);
							return success;
						default:
							//std::cout << *head << '\n';
							ProcError(&head);
							return false;
					}
				} else {
					std::cout << "No match: " << head << '\n';
					return false;
				}
			}
		}
	}
}

inline SlangObj* SlangInterpreter::StackAlloc(){
	if (frameIndex==stack.size()-1)
		return nullptr;
		//stack.emplace_back();
	
	gMaxStackHeight = (gMaxStackHeight<frameIndex) ? frameIndex : gMaxStackHeight;
	return &stack[frameIndex++];
}

inline void SlangInterpreter::StackAllocFrame(){
	frameStack.emplace_back(baseIndex);
	baseIndex = frameIndex;
}

inline void SlangInterpreter::StackFreeFrame(){
	assert(!frameStack.empty());
	frameIndex = baseIndex;
	baseIndex = frameStack.back();
	frameStack.pop_back();
}

void SlangInterpreter::PushError(const SlangObj* expr,const std::string& msg){
	LocationData loc = parser.GetExprLocation(expr);
	errors.emplace_back(loc,msg);
}

void SlangInterpreter::EvalError(const SlangObj* expr){
	//if (expr==program) return;
	std::stringstream msg = {};
	msg << "EvalError:\n    Error while evaluating '" << *expr << "'";
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
		case SlangType::Null:
			return "Null";
		case SlangType::Lambda:
			return "Lambda";
	}
	return nullptr;
}

void SlangInterpreter::TypeError(const SlangObj* expr,SlangType found,SlangType expect){
	std::stringstream msg = {};
	msg << "TypeError:\n    Expected type " << 
		TypeToString(expect) << " instead of type " << 
		TypeToString(found) << " in argument '" << *expr << "'";
		
	PushError(expr,msg.str());
}

void SlangInterpreter::ProcError(const SlangObj* proc){
	std::stringstream msg = {};
	msg << "ProcError:\n    Procedure '" << *proc << "' is not defined!";
	
	PushError(proc,msg.str());
}

void SlangInterpreter::ArityError(const SlangObj* head,size_t found,size_t expect){
	std::stringstream msg = {};
	msg << "ArityError:\n    Procedure '" << *head << 
		"' takes " << expect << " arguments, was given " <<
		found;
		
	PushError(head,msg.str());
}

void SlangInterpreter::Run(SlangObj* prog){
	arena = new MemArena();
	gDebugInterpreter = this;
	stack = {};
	frameStack = {};
	frameStack.reserve(32);
	stack.resize(5461);
	frameIndex = 0;
	baseIndex = 0;
	errors = {};
	
	program = prog;
	
	env = {};
	SlangObj res;
	WrappedEvalExpr(prog,&res,&env);
	
	
	if (!errors.empty()){
		PrintErrors(errors);
		std::cout << '\n';
		//for (const auto& e : errors){
		//	std::cout << e.msg << '\n';
		//}
	}
	//std::cout << res << '\n';
	//FreeExpr(prog);
	//FreeExpr(res);
	
	//FreeEnv(&env);
	
	//std::cout << "still alive:\n";
	//for (auto* alive : stillAround){
	//	std::cout << *alive <<'\n';
	//}
	
	std::cout << "Alloc balance: " << gAllocCount << '\n';
	std::cout << "Alloc total: " << gAllocTotal << " (" << gAllocTotal*sizeof(SlangObj)/1024 << " KB)\n";
	std::cout << "Heap alloc total: " << gHeapAllocTotal << " (" << gHeapAllocTotal*sizeof(SlangObj)/1024 << " KB)\n";
	std::cout << "Max allocated: " << gMaxAlloc << " (" << gMaxAlloc*sizeof(SlangObj)/1024 << " KB)\n";
	std::cout << "Eval count: " << gEvalCounter << '\n';
	std::cout << "Non-eliminated eval count: " << gEvalRecurCounter << '\n';
	std::cout << "Frame height at end: " << frameStack.size() << '\n';
	std::cout << "Max stack height: " << gMaxStackHeight << '\n';
	delete arena;
}

inline SlangToken SlangTokenizer::NextToken(){
	while (pos!=end&&
			(*pos==' '||*pos=='\t'||*pos=='\n')){
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
	if (pos+1!=end) nextC = *(pos+1);
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
	} else if (c==';'){
		token.type = SlangTokenType::Comment;
		while (pos!=end&&*pos!='\n') ++pos;
	} else if ((c=='-'&&nextC>='0'&&nextC<='9')||(c>='0'&&c<='9')){
		token.type = SlangTokenType::Int;
		while (pos!=end&&*pos>='0'&&*pos<='9'){
			++pos;
		}
		// if unexpected char shows up at end (like '123q')
		if (pos!=end&&
			*pos!=' '&&*pos!=')'&&*pos!=']'&&*pos!='}'&&
			*pos!='\t'&&*pos!='\n'){
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
	return new SlangObj;
}

inline SlangObj* QuoteExpr(SlangObj* expr){
	SlangObj* q = HeapAllocObj();
	q->type = SlangType::Symbol;
	q->symbol = SLANG_QUOTE;

	SlangObj* list = HeapAllocObj();
	list->type = SlangType::List;
	list->left = q;
	list->right = HeapAllocObj();
	list->right->type = SlangType::List;
	list->right->left = expr;
	list->right->right = HeapAllocObj();
	list->right->right->type = SlangType::Null;

	return list;
}

inline SlangObj* SlangParser::ParseObj(){
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
			return intObj;
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
			return sym;
		}
		case SlangTokenType::True: {
			NextToken();
			SlangObj* boolObj = HeapAllocObj();
			boolObj->type = SlangType::Bool;
			boolObj->boolean = true;
			codeMap[boolObj] = loc;
			return boolObj;
		}
		case SlangTokenType::False: {
			NextToken();
			SlangObj* boolObj = HeapAllocObj();
			boolObj->type = SlangType::Bool;
			boolObj->boolean = false;
			codeMap[boolObj] = loc;
			return boolObj;
		}
		case SlangTokenType::Quote: {
			NextToken();
			SlangObj* res = QuoteExpr(ParseExprOrObj());
			codeMap[res] = loc;
			return res;
		}
		case SlangTokenType::LeftBracket: {
			SlangObj* res = ParseExpr();
			codeMap[res] = loc;
			return res;
		}
		case SlangTokenType::Error:
			PushError("Token error: "+std::string(token.view));
			return nullptr;
		case SlangTokenType::EndOfFile:
			// might be superfluous
			PushError("Ran out of tokens!");
			return nullptr;
		default:
			PushError("Unexpected token: "+std::string(token.view));
			return nullptr;
	}
	
	PushError("Error parsing object!");
	return nullptr;
}

SlangObj* SlangParser::ParseExpr(){
	SlangObj* objHead = HeapAllocObj();
	objHead->type = SlangType::Null;
	
	SlangObj* obj = objHead;
	char leftBracket = token.view[0];
	NextToken();
	
	while (token.type!=SlangTokenType::RightBracket){
		obj->left = ParseObj();
		if (!obj->left) return nullptr;
		
		obj->type = SlangType::List;
		
		obj->right = HeapAllocObj();
		obj = obj->right;
		obj->type = SlangType::Null;
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
		return nullptr;
	}
	
	NextToken();
	return objHead;
}

inline SlangObj* SlangParser::ParseExprOrObj(){
	if (token.type==SlangTokenType::LeftBracket){
		LocationData loc = {token.line,token.col,0};
		SlangObj* res = ParseExpr();
		codeMap[res] = loc;
		return res;
	}
	SlangObj* res = ParseObj();
	return res;
}

// wraps program exprs in a (do ...) block
inline SlangObj* WrapProgram(const std::vector<SlangObj*>& exprs){
	SlangObj* doSym = HeapAllocObj();
	doSym->type = SlangType::Symbol;
	doSym->symbol = SLANG_DO;

	SlangObj* outer = HeapAllocObj();
	outer->type = SlangType::List;
	outer->left = doSym;
	
	
	SlangObj* next = HeapAllocObj();
	next->type = SlangType::Null;
	outer->right = next;
	for (auto* expr : exprs){
		next->left = expr;
		next->type = SlangType::List;
		next->right = HeapAllocObj();
		next = next->right;
		next->type = SlangType::Null;
	}
	
	// add one last null element for error reporting
	SlangObj* last = HeapAllocObj();
	last->type = SlangType::Null;
	next->type = SlangType::List;
	next->left = last;
	next->right = HeapAllocObj();
	next->right->type = SlangType::Null;
	
	return outer;
}

SlangObj* SlangParser::Parse(const std::string& code){
	gDebugParser = this;
	tokenizer.SetText(code);
	token.type = SlangTokenType::Comment;
	//nameDict = gDefaultNameDict;
	//currentName = GLOBAL_SYMBOL_COUNT;
	NextToken();
	
	std::vector<SlangObj*> exprs = {};
	while (errors.empty()){
		if (token.type==SlangTokenType::Quote){
			NextToken();
			exprs.push_back(QuoteExpr(ParseExprOrObj()));
		} else {
			exprs.push_back(ParseExprOrObj());
		}
		
		if (exprs.back()==nullptr) break;
		if (token.type==SlangTokenType::EndOfFile) break;
	}
	
	if (!errors.empty()){
		PrintErrors(errors);
		return nullptr;
	}
	return WrapProgram(exprs);
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

SlangObj* SlangInterpreter::Parse(const std::string& code){
	return parser.Parse(code);
}

std::ostream& operator<<(std::ostream& os,SlangObj obj){
	switch (obj.type){
		case SlangType::Int:
			os << obj.integer;
			break;
		case SlangType::Real:
			os << obj.real;
			break;
		case SlangType::Lambda: {
			os << "(& (";
			SlangLambda* l = gDebugInterpreter->GetLambda(obj.lambda);
			for (size_t i=0;i<l->params.size();++i){
				if (i!=0)
					os << ' ';
				os << gDebugParser->GetSymbolString(l->params[i]);
			}
			os << ") ";
			os << *l->body << ")";
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
			//if (obj.right->type!=SlangType::List&&obj.right->type!=SlangType::Null){
			//	os << '[' << *obj.left << ' ' << *obj.right << ']';
			//} else {
				os << '(';
				os << *obj.left;
				SlangObj* next = obj.right;
				while (next->type!=SlangType::Null){
					os << ' ';
					if(next->type!=SlangType::List){
						os << ". " << *next;
						break;
					}
					os << *next->left;
					next = next->right;
				}
				os << ')';
			//}
			break;
		}
		case SlangType::Null:
			os << "()";
			break;
			
	}
	
	return os;
}

}
