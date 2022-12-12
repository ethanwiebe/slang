#include "slang.h"

#include <assert.h>
#include <iostream>

namespace slang {

struct ErrorData {
	size_t line,col;
	std::string message;
};

enum class SlangTokenType : uint8_t {
	Symbol = 0,
	Int,
	Real,
	LeftBracket,
	RightBracket,
	EndOfFile,
	Error
};

struct SlangToken {
	SlangTokenType type;
	std::string_view view;
};

StringIt gParseIt,gParseEnd;
size_t gCurrentLine = 0;
size_t gCurrentCol = 0;

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

inline SlangToken GetToken(){
	while (gParseIt!=gParseEnd&&
			(*gParseIt==' '||*gParseIt=='\t'||*gParseIt=='\n')){
		if (*gParseIt=='\n'){
			++gCurrentLine;
			gCurrentCol = 0;
		} else {
			++gCurrentCol;
		}
		++gParseIt;
	}
	
	if (gParseIt==gParseEnd) return {SlangTokenType::EndOfFile,{}};
	
	StringIt begin = gParseIt;
	char c = *begin;
	char nextC = ' ';
	if (gParseIt+1!=gParseEnd) nextC = *(gParseIt+1);
	SlangToken tok = {};
	
	if (c=='('||c=='['||c=='{'){
		tok.type = SlangTokenType::LeftBracket;
		++gParseIt;
	} else if (c==')'||c==']'||c=='}'){
		tok.type = SlangTokenType::RightBracket;
		++gParseIt;
	} else if ((c=='-'&&nextC>='0'&&nextC<='9')||(c>='0'&&c<='9')){
		tok.type = SlangTokenType::Int;
		while (gParseIt!=gParseEnd&&*gParseIt>='0'&&*gParseIt<='9'){
			++gParseIt;
		}
		// if unexpected char shows up at end (like '123q')
		if (gParseIt!=gParseEnd&&
			*gParseIt!=' '&&*gParseIt!=')'&&*gParseIt!=']'&&*gParseIt!='}'&&
			*gParseIt!='\t'&&*gParseIt!='\n'){
			tok.type = SlangTokenType::Error;
			++gParseIt;
		}
	} else if (IsIdentifierChar(c)){
		tok.type = SlangTokenType::Symbol;
		while (gParseIt!=gParseEnd&&IsIdentifierChar(c)){
			++gParseIt;
			c = *gParseIt;
		}
	} else {
		tok.type = SlangTokenType::Error;
		++gParseIt;
	}
	tok.view = {begin,gParseIt};
	gCurrentCol += gParseIt-begin;
	return tok;
}

std::string gFileName = "repl";
std::string gParseText = {};
std::vector<ErrorData> gErrors = {};
SlangToken gToken;

void PushError(const std::string& msg){
	gErrors.emplace_back(gCurrentLine,gCurrentCol,msg);
}

void NextToken(){
	if (gToken.type==SlangTokenType::EndOfFile){
		PushError("Ran out of tokens while parsing!");
	}
	
	gToken = GetToken();
	
	if (gToken.type==SlangTokenType::Error){
		PushError("Unexpected character: "+std::string(gToken.view)+"\n");
	}
}

enum SlangGlobalSymbol {
	SLANG_DEFINE = 0,
	SLANG_LAMBDA,
	SLANG_IF,
	SLANG_DO,
	SLANG_ADD,
	SLANG_SUB,
	SLANG_MUL,
	GLOBAL_SYMBOL_COUNT
};

SymbolNameDict gSymbolDict = {
	{"def",SLANG_DEFINE},
	{"lambda",SLANG_LAMBDA},
	{"if",SLANG_IF},
	{"do",SLANG_DO},
	{"+",SLANG_ADD},
	{"-",SLANG_SUB},
	{"*",SLANG_MUL}
};

std::vector<SlangLambda> gLambdaDict = {};

SymbolName gCurrentSymbolName = GLOBAL_SYMBOL_COUNT;

SymbolName RegisterSymbol(const std::string& symbol){
	SymbolName name = gCurrentSymbolName++;
	
	gSymbolDict[symbol] = name;
	return name;
}

LambdaKey RegisterLambda(const SlangLambda& lam){
	gLambdaDict.push_back(lam);
	return gLambdaDict.size()-1;
}

SlangLambda* GetLambda(LambdaKey key){
	return &gLambdaDict[key];
}

std::string GetSymbolString(SymbolName symbol){
	for (const auto& [key,val] : gSymbolDict){
		if (val==symbol)
			return key;
	}
	
	return "UNDEFINED";
}

void Env::DefSymbol(SymbolName name,SlangObj* obj){
	symbolMap.insert_or_assign(name,obj);
}

SlangObj* Env::GetSymbol(SymbolName name) const {
	if (!symbolMap.contains(name))
		return nullptr;
		
	return symbolMap.at(name);
}

void Copy(const SlangObj* expr,SlangObj* into){
	into->type = expr->type;
	into->left = expr->left;
	into->right = expr->right;
	if (expr->type==SlangType::List){
		Copy(expr->left,into->left);
		Copy(expr->right,into->right);
	}
}

SlangObj* EvalExpr(const SlangObj* expr,Env& env){
	switch (expr->type){
		case SlangType::Real:
		case SlangType::Null:
		case SlangType::Int:
		case SlangType::String:
		case SlangType::Bool:
		case SlangType::Lambda: {
			SlangObj* n = AllocateObj();
			Copy(expr,n);
			return n;
		}
		case SlangType::Symbol: {
			Env* parent = &env;
			while (parent){
				SlangObj* ref = parent->GetSymbol(expr->symbol);
				if (ref)
					return ref;
				parent = parent->parent;
			}
			SlangObj* n = AllocateObj();
			Copy(expr,n);
			return n;
		}
		
		// function
		case SlangType::List: {
			SlangObj* head = EvalExpr(expr->left,env);
			const SlangObj* argIt = expr;
			
			// for lambda 
			std::vector<SymbolName> params = {};
			SlangObj* lambdaBody = nullptr;
			// for all funcs
			std::vector<SlangObj*> args = {};
			
			if (head->type==SlangType::Symbol&&head->symbol==SLANG_LAMBDA){
				// parse lambda args (params, body)
				assert(argIt->right->type==SlangType::List);
				assert(argIt->right->left->type==SlangType::List||argIt->right->left->type==SlangType::Null);
				SlangObj* paramList = argIt->right->left;
				while (paramList->type!=SlangType::Null){
					// params must be list of symbols
					assert(paramList->left->type==SlangType::Symbol);
					params.push_back(paramList->left->symbol);
					paramList = paramList->right;
				}
				lambdaBody = argIt->right->right->left;
				// only two args
				assert(argIt->right->right->right->type==SlangType::Null);
			} else if (head->type==SlangType::Symbol&&head->symbol==SLANG_DEFINE){
				assert(argIt->right->type==SlangType::List);
				assert(argIt->right->left->type==SlangType::Symbol);
				args.push_back(argIt->right->left);
				args.push_back(EvalExpr(argIt->right->right->left,env));
				// only two args
				assert(argIt->right->right->right->type==SlangType::Null);
			} else {
				// just parse args
				while (true){
					argIt = argIt->right;
					if (argIt->type==SlangType::Null) break;
					SlangObj* a = argIt->left;
					args.push_back(a);
				}
			}
			
			if (head->type==SlangType::Lambda){
				SlangLambda* lam = GetLambda(head->lambda);
				assert(args.size()==lam->params.size());
				
				Env* funcEnv = lam->env;
				for (size_t i=0;i<args.size();++i){
					args[i] = EvalExpr(args[i],env);
				}
				
				for (size_t i=0;i<lam->params.size();++i){
					funcEnv->DefSymbol(lam->params[i],args[i]);
				}
				
				return EvalExpr(lam->body,*funcEnv);
			} else if (head->type==SlangType::Symbol){
				// handle predefined symbols
				switch (head->symbol){
					case SLANG_DEFINE:
						assert(args.size()==2);
						assert(args[0]->type==SlangType::Symbol);
						env.DefSymbol(args[0]->symbol,args[1]);
						return args[1];
					case SLANG_LAMBDA: {
						Env* child = env.MakeChild();
						SlangLambda lam = {params,lambdaBody,child};
						LambdaKey key = RegisterLambda(lam);
						SlangObj* ret = AllocateObj();
						ret->type = SlangType::Lambda;
						ret->lambda = key;
						return ret;
					}
					case SLANG_DO: {
						SlangObj* res;
						for (const auto& arg : args){
							res = EvalExpr(arg,env);
						}
						return res;
					}
					case SLANG_ADD: {
						int64_t sum = 0;
						for (const auto& arg : args){
							SlangObj* e = EvalExpr(arg,env);
							assert(e->type==SlangType::Int);
							sum += e->integer;
						}
						SlangObj* s = AllocateObj();
						s->type = SlangType::Int;
						s->integer = sum;
						return s;
					}
					case SLANG_SUB: {
						SlangObj* start = EvalExpr(args.front(),env);
						assert(start->type==SlangType::Int);
						int64_t diff = start->integer;
						for (size_t i=1;i<args.size();++i){
							SlangObj* e = EvalExpr(args[i],env);
							assert(e->type==SlangType::Int);
							diff -= e->integer;
						}
						SlangObj* s = AllocateObj();
						s->type = SlangType::Int;
						s->integer = diff;
						return s;
					}
					case SLANG_MUL: {
						int64_t prod = 1;
						for (const auto& arg : args){
							SlangObj* e = EvalExpr(arg,env);
							assert(e->type==SlangType::Int);
							prod *= e->integer;
						}
						SlangObj* s = AllocateObj();
						s->type = SlangType::Int;
						s->integer = prod;
						return s;
					}
					case SLANG_IF: {
						assert(args.size()==3);
						bool test;
						
						auto* first = EvalExpr(args.front(),env);
						if (first->type==SlangType::Int){
							test = (bool)first->integer;
						} else if (first->type==SlangType::Bool){
							test = first->boolean;
						} else {
							assert(false);
							test = false;
						}
						
						if (test){
							return EvalExpr(args[1],env);
						}
						return EvalExpr(args[2],env);
					}
					default:
						assert(false);
						break;
				}
			} else {
				assert(false);
			}
			
			break;
		}
	}

	SlangObj* null = AllocateObj();
	null->type = SlangType::Null;
	return null;
}

SlangObj* AllocateObj(){
	SlangObj* p = (SlangObj*)malloc(sizeof(SlangObj));
	return p;
}

void FreeObj(SlangObj* p){
	free(p);
}

void FreeExpr(SlangObj* p){
	if (p->type!=SlangType::List){
		free(p);
	} else {
		FreeExpr(p->left);
		FreeExpr(p->right);
	}
}

SlangObj MakeInt(int64_t i){
	SlangObj obj = {};
	obj.type = SlangType::Int;
	obj.integer = i;
	return obj;
}

SlangObj MakeSymbol(SymbolName symbol){
	SlangObj obj = {};
	obj.type = SlangType::Symbol;
	obj.symbol = symbol;
	return obj;
}

SlangObj* MakeList(std::vector<SlangObj>& objs){
	SlangObj* listHead = AllocateObj();
	listHead->type = SlangType::List;
	SlangObj* list = listHead;
	for (auto& obj : objs){
		list->left = &obj;
		list->right = AllocateObj();
		list = list->right;
		list->type = SlangType::List;
	}
	list->type = SlangType::Null;
	return listHead;
}

void ResetParseState(){
	gCurrentLine = 0;
	gCurrentCol = 0;
	gErrors = {};
}

SlangObj* ParseExpr();

inline SlangObj* ParseObj(){
	switch (gToken.type){
		case SlangTokenType::Int: {
			SlangObj* obj = AllocateObj();
			obj->type = SlangType::Int;
			char* d;
			obj->integer = strtoll(gToken.view.begin(),&d,10);
			NextToken();
			return obj;
		}
		case SlangTokenType::Symbol: {
			SlangObj* obj = AllocateObj();
			obj->type = SlangType::Symbol;
			std::string copy = std::string(gToken.view);
			if (!gSymbolDict.contains(copy))
				obj->symbol = RegisterSymbol(copy);
			else
				obj->symbol = gSymbolDict.at(copy);
			NextToken();
			return obj;
		}
		case SlangTokenType::LeftBracket:
			return ParseExpr();
		case SlangTokenType::Error:
			PushError("Token error: "+std::string(gToken.view));
			return nullptr;
		case SlangTokenType::EndOfFile:
			PushError("Ran out of tokens!");
			return nullptr;
		default:
			PushError("Unexpected token: "+std::string(gToken.view));
			return nullptr;
	}
	
	PushError("Error parsing object!");
	return nullptr;
}

SlangObj* ParseExpr(){
	SlangObj* objHead = AllocateObj();
	objHead->type = SlangType::Null;
	
	SlangObj* obj = objHead;
	assert(gToken.type==SlangTokenType::LeftBracket);
	char leftBracket = gToken.view[0];
	NextToken();
	
	while (gToken.type!=SlangTokenType::RightBracket){
		obj->left = ParseObj();
		if (!obj->left) return nullptr;
		
		obj->type = SlangType::List;
		
		obj->right = AllocateObj();
		obj = obj->right;
		obj->type = SlangType::Null;
	}
	
	char rightBracket = gToken.view[0];
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


void PrintErrors(){
	for (const auto& error : gErrors){
		std::cout << gFileName << ':' << error.line << ',' << error.col <<
			": " << error.message << '\n';
	}
}

void Parse(const std::string& code){
	ResetParseState();
	gParseText = code;
	gParseIt = code.begin();
	gParseEnd = code.end();
	gToken = GetToken();
	
	Env e = {};
	std::vector<SlangObj*> exprs = {};
	
	while (gErrors.empty()){
		if (gToken.type==SlangTokenType::LeftBracket)
			exprs.push_back(ParseExpr());
		else
			exprs.push_back(ParseObj());
			
		if (exprs.back()==nullptr) break;
		if (gToken.type==SlangTokenType::EndOfFile) break;
	}
	
	if (!gErrors.empty()){
		PrintErrors();
	} else {
		for (SlangObj* expr : exprs){
			std::cout << *expr << '\n';
			SlangObj* res = EvalExpr(expr,e);
			std::cout << "  " << *res << '\n';
		}
		for (SlangObj* expr : exprs){
			FreeExpr(expr);
		}
	}
	
	
}

std::ostream& operator<<(std::ostream& os,SlangObj obj){
	switch (obj.type){
		case SlangType::Int:
			os << obj.integer;
			break;
		case SlangType::Real:
			os << obj.real;
			break;
		case SlangType::Lambda:
			os << "#COMPOUND PROCEDURE";
			break;
		case SlangType::Symbol:
			os << GetSymbolString(obj.symbol);
			break;
		case SlangType::String:
			os << obj.character;
			break;
		case SlangType::Bool:
			if (obj.boolean){
				os << "#t";
			} else {
				os << "#f";
			}
			break;
		case SlangType::List: {
			os << '(';
			os << *obj.left;
			SlangObj* next = obj.right;
			while (next->type!=SlangType::Null){
				os << ' ';
				os << *next->left;
				next = next->right;
			}
			os << ')';
			break;
		}
		case SlangType::Null:
			os << "()";
			break;
			
	}
	
	return os;
}

}
