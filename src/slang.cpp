#include "slang.h"

#include <assert.h>
#include <iostream>
#include <cstring>

#include <set>

namespace slang {

size_t gAllocCount = 0;
size_t gAllocTotal = 0;
size_t gMaxAlloc = 0;
std::set<SlangObj*> stillAround = {};

size_t gEvalCounter = 0;
size_t gEvalRecurCounter = 0;

SlangParser* gDebugParser;
SlangInterpreter* gDebugInterpreter;

inline SlangObj* AllocateObj(){
	SlangObj* p = (SlangObj*)malloc(sizeof(SlangObj));
	++gAllocCount;
	++gAllocTotal;
	gMaxAlloc = (gAllocCount>gMaxAlloc) ? gAllocCount : gMaxAlloc;
	//stillAround.insert(p);
	return p;
}

inline void FreeObj(SlangObj* p){
	/*if (!stillAround.contains(p)){
		std::cout << "double free!\n";
	} else {
		stillAround.erase(p);
	}*/
	free(p);
	--gAllocCount;
}

void FreeExpr(SlangObj* p){
	if (p->type==SlangType::List){
		FreeExpr(p->left);
		FreeExpr(p->right);
	}
	FreeObj(p);
}

void FreeEnv(Env* e){
	for (auto* child : e->children){
		FreeEnv(child);
		delete child;
	}
	
	for (auto [key, val] : e->symbolMap){
		FreeExpr(val);
	}
}

SlangObj* Copy(const SlangObj* expr){
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
	SLANG_ADD,
	SLANG_SUB,
	SLANG_MUL,
	GLOBAL_SYMBOL_COUNT
};

SymbolNameDict gDefaultNameDict = {
	{"def",SLANG_DEFINE},
	{"lambda",SLANG_LAMBDA},
	{"set!",SLANG_SET},
	{"if",SLANG_IF},
	{"do",SLANG_DO},
	{"quote",SLANG_QUOTE},
	{"pair",SLANG_PAIR},
	{"L",SLANG_LEFT},
	{"R",SLANG_RIGHT},
	{"+",SLANG_ADD},
	{"-",SLANG_SUB},
	{"*",SLANG_MUL}
};

void Env::DefSymbol(SymbolName name,SlangObj* obj){
	if (symbolMap.contains(name))
		FreeExpr(symbolMap.at(name));
	symbolMap.insert_or_assign(name,obj);
}

SlangObj* Env::GetSymbol(SymbolName name) const {
	if (!symbolMap.contains(name))
		return nullptr;
		
	return symbolMap.at(name);
}
inline Env* Env::MakeChild(){
	Env* child = new Env;
	*child = {{},{},this};
	if (parent){
		for (const auto& [name,sym] : symbolMap){
			child->DefSymbol(name,Copy(sym));
		}
	}
	children.push_back(child);
	return child;
}

inline void Env::Clear(){
	for (const auto& [name,sym] : symbolMap){
		FreeExpr(sym);
	}
	symbolMap.clear();
}

inline SlangObj* MakeInt(int64_t i){
	SlangObj* obj = AllocateObj();
	obj->type = SlangType::Int;
	obj->integer = i;
	return obj;
}

inline SlangObj* MakeBool(bool b){
	SlangObj* obj = AllocateObj();
	obj->type = SlangType::Bool;
	obj->boolean = b;
	return obj;
}

inline SlangObj* MakeSymbol(SymbolName symbol){
	SlangObj* obj = AllocateObj();
	obj->type = SlangType::Symbol;
	obj->symbol = symbol;
	return obj;
}

inline SlangObj* MakeList(const std::vector<SlangObj*>& objs){
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

inline bool SetRecursiveSymbol(Env* e,SymbolName name,SlangObj* val){
	while (e){
		SlangObj* res = e->GetSymbol(name);
		if (res){
			e->DefSymbol(name,Copy(val));
			return true;
		}
		e = e->parent;
	}
	return false;
}

inline SlangObj* SlangInterpreter::SlangFuncDefine(const SlangArgVec& args,Env* env){
	assert(args.size()==2);
	assert(args[0]->type==SlangType::Symbol);
	// can't exist
	//assert(env->GetSymbol(args[0]->symbol)==nullptr);
	
	SlangObj* res = EvalExpr(args[1],env);
	// not nullptr
	//assert(res);
	
	env->DefSymbol(args[0]->symbol,res);
	return Copy(res);
}

inline SlangObj* SlangInterpreter::SlangFuncLambda(const SlangArgVec& args,Env* env){
	assert(args.size()==2);
	assert(args[0]->type==SlangType::List||args[0]->type==SlangType::Null);
	std::vector<SymbolName> params = {};
	std::set<SymbolName> seen = {};
	SlangObj* paramIt = args[0];
	while (paramIt->type!=SlangType::Null){
		assert(paramIt->type==SlangType::List);
		assert(paramIt->left->type==SlangType::Symbol);
		assert(!seen.contains(paramIt->left->symbol));
		
		params.push_back(paramIt->left->symbol);
		seen.insert(paramIt->left->symbol);
		
		paramIt = paramIt->right;
	}

	Env* child = env->MakeChild();
	SlangLambda lam = {params,args[1],child};
	LambdaKey key = RegisterLambda(lam);
	SlangObj* ret = AllocateObj();
	ret->type = SlangType::Lambda;
	ret->lambda = key;
	return ret;
}

inline SlangObj* SlangInterpreter::SlangFuncSet(const SlangArgVec& args,Env* env){
	assert(args.size()==2);
	assert(args[0]->type==SlangType::Symbol);
	SlangObj* s = EvalExpr(args[1],env);
	assert(s);
	if (!SetRecursiveSymbol(env,args[0]->symbol,s)){
		assert(false);
	}
	SlangObj* copy = Copy(s);
	FreeExpr(s);
	return copy;
}

inline SlangObj* SlangInterpreter::SlangFuncQuote(const SlangArgVec& args){
	assert(args.size()==1);
	return Copy(args.front());
}

inline SlangObj* SlangInterpreter::SlangFuncPair(const SlangArgVec& args,Env* env){
	assert(args.size()==2);
	SlangObj* obj = AllocateObj();
	
	obj->type = SlangType::List;
	obj->left = EvalExpr(args.front(),env);
	obj->right = EvalExpr(args.back(),env);
	return obj;
}

inline SlangObj* SlangInterpreter::SlangFuncLeft(const SlangArgVec& args,Env* env){
	assert(args.size()==1);
	SlangObj* m = EvalExpr(args.front(),env);
	assert(m->type==SlangType::List);
	SlangObj* copy = Copy(m->left);
	FreeExpr(m);
	return copy;
}

inline SlangObj* SlangInterpreter::SlangFuncRight(const SlangArgVec& args,Env* env){
	assert(args.size()==1);
	SlangObj* m = EvalExpr(args.front(),env);
	assert(m->type==SlangType::List);
	SlangObj* copy = Copy(m->right);
	FreeExpr(m);
	return copy;
}

/*inline SlangObj* SlangInterpreter::SlangFuncSetLeft(const SlangArgVec& args,Env* env){
	assert(args.size()==2);
	SlangObj* toSet = EvalExpr(args[0],env);
	assert(toSet->type==SlangType::List);
	
	SlangObj* s = EvalExpr(args[1],env);
	
	SlangObj* copy = Copy(s);
	FreeExpr(s);
	return copy;
}*/

inline SlangObj* SlangInterpreter::SlangFuncAdd(const SlangArgVec& args,Env* env){
	int64_t sum = 0;
	for (const auto& arg : args){
		SlangObj* e = EvalExpr(arg,env);
		assert(e->type==SlangType::Int);
		sum += e->integer;
		FreeObj(e);
	}
	return MakeInt(sum);
}

inline SlangObj* SlangInterpreter::SlangFuncSub(const SlangArgVec& args,Env* env){
	SlangObj* start = EvalExpr(args.front(),env);
	assert(start->type==SlangType::Int);
	int64_t diff = start->integer;
	FreeObj(start);
	for (size_t i=1;i<args.size();++i){
		SlangObj* e = EvalExpr(args[i],env);
		assert(e->type==SlangType::Int);
		diff -= e->integer;
		FreeObj(e);
	}
	return MakeInt(diff);
}

inline SlangObj* SlangInterpreter::SlangFuncMul(const SlangArgVec& args,Env* env){
	int64_t prod = 1;
	for (const auto& arg : args){
		SlangObj* e = EvalExpr(arg,env);
		assert(e->type==SlangType::Int);
		prod *= e->integer;
		FreeObj(e);
	}
	return MakeInt(prod);
}

SlangObj* SlangInterpreter::EvalExpr(const SlangObj* expr,Env* env){
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
				return Copy(expr);
			}
			case SlangType::Symbol: {
				Env* parent = env;
				SlangObj* ref = nullptr;
				if (GetRecursiveSymbol(parent,expr->symbol,&ref)){
					return Copy(ref);
				}
				
				return Copy(expr);
			}
			
			// function
			case SlangType::List: {
				SlangObj* head = EvalExpr(expr->left,env);
				const SlangObj* argIt = expr->right;
				
				std::vector<SlangObj*> args{};
				args.reserve(4);
				while (argIt->type!=SlangType::Null){
					SlangObj* a = argIt->left;
					args.push_back(a);
					argIt = argIt->right;
				}
				
				if (head->type==SlangType::Lambda){
					SlangLambda* lam = GetLambda(head->lambda);
					assert(args.size()==lam->params.size());
					
					Env* funcEnv = lam->env;
					for (size_t i=0;i<args.size();++i){
						args[i] = EvalExpr(args[i],env);
					}
					
					//funcEnv->Clear();
					for (size_t i=0;i<lam->params.size();++i){
						funcEnv->DefSymbol(lam->params[i],Copy(args[i]));
					}
					
					for (auto* arg : args){
						FreeExpr(arg);
					}
					
					FreeExpr(head);
					expr = lam->body;
					env = funcEnv;
					continue;
				} else if (head->type==SlangType::Symbol){
					auto symbol = head->symbol;
					FreeObj(head);
					// handle predefined symbols
					switch (symbol){
						case SLANG_DEFINE:
							return SlangFuncDefine(args,env);
						case SLANG_LAMBDA:
							return SlangFuncLambda(args,env);
						case SLANG_SET:
							return SlangFuncSet(args,env);
						case SLANG_DO: {
							SlangObj* res = nullptr;
							for (size_t i=0;i<args.size()-1;++i){
								if (res) FreeExpr(res);
								res = EvalExpr(args[i],env);
								assert(res);
							}
							if (res) FreeExpr(res);
							
							expr = args.back();
							continue;
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
							FreeExpr(first);
							
							if (test){
								expr = args[1];
								continue;
							}
							expr = args[2];
							continue;
						}
						case SLANG_QUOTE:
							return SlangFuncQuote(args);
						case SLANG_PAIR:
							return SlangFuncPair(args,env);
						case SLANG_LEFT:
							return SlangFuncLeft(args,env);
						case SLANG_RIGHT:
							return SlangFuncRight(args,env);
						case SLANG_ADD:
							return SlangFuncAdd(args,env);
						case SLANG_SUB:
							return SlangFuncSub(args,env);
						case SLANG_MUL:
							return SlangFuncMul(args,env);
						default:
							//std::cout << *head << '\n';
							assert(false);
							break;
					}
				} else {
					std::cout << "No match: " << *head << '\n';
					assert(false);
				}
			}
		}
	}
}

void SlangInterpreter::Run(SlangObj* prog){
	gDebugInterpreter = this;
	
	program = prog;
	env = {};
	SlangObj* res = EvalExpr(prog,&env);
	
	std::cout << *res << '\n';
	FreeExpr(prog);
	FreeExpr(res);
	
	FreeEnv(&env);
	
	//std::cout << "still alive:\n";
	//for (auto* alive : stillAround){
	//	std::cout << *alive <<'\n';
	//}
	
	std::cout << "Alloc balance: " << gAllocCount << '\n';
	std::cout << "Alloc total: " << gAllocTotal << " (" << gAllocTotal*sizeof(SlangObj)/1024 << " KB)\n";
	std::cout << "Max allocated: " << gMaxAlloc << " (" << gMaxAlloc*sizeof(SlangObj)/1024 << " KB)\n";
	std::cout << "Eval count: " << gEvalCounter << '\n';
	std::cout << "Non-eliminated eval count: " << gEvalRecurCounter << '\n';
	
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
	errors.emplace_back(token.line,token.col,msg);
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

inline SlangObj* QuoteExpr(SlangObj* expr){
	return MakeList({MakeSymbol(SLANG_QUOTE),expr});
}

inline SlangObj* SlangParser::ParseObj(){
	switch (token.type){
		case SlangTokenType::Int: {
			char* d;
			int64_t i = strtoll(token.view.begin(),&d,10);
			NextToken();
			return MakeInt(i);
		}
		case SlangTokenType::Symbol: {
			SymbolName s;
			std::string copy = std::string(token.view);
			if (!nameDict.contains(copy))
				s = RegisterSymbol(copy);
			else
				s = nameDict.at(copy);
			NextToken();
			return MakeSymbol(s);
		}
		case SlangTokenType::True: {
			NextToken();
			return MakeBool(true);
		}
		case SlangTokenType::False: {
			NextToken();
			return MakeBool(false);
		}
		case SlangTokenType::Quote:
			NextToken();
			return QuoteExpr(ParseExprOrObj());
		case SlangTokenType::LeftBracket:
			return ParseExpr();
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
	SlangObj* objHead = AllocateObj();
	objHead->type = SlangType::Null;
	
	SlangObj* obj = objHead;
	assert(token.type==SlangTokenType::LeftBracket);
	char leftBracket = token.view[0];
	NextToken();
	
	while (token.type!=SlangTokenType::RightBracket){
		obj->left = ParseObj();
		if (!obj->left) return nullptr;
		
		obj->type = SlangType::List;
		
		obj->right = AllocateObj();
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
		return ParseExpr();
	}
	return ParseObj();
}

void PrintErrors(const SlangParser& parser){
	for (const auto& error : parser.errors){
		std::cout << "@" << error.line << ',' << error.col <<
			": " << error.message << '\n';
	}
}

// wraps program exprs in a (do ...) block
inline SlangObj* WrapProgram(const std::vector<SlangObj*>& exprs){
	SlangObj* outer = AllocateObj();
	outer->type = SlangType::List;
	outer->left = MakeSymbol(SLANG_DO);
	outer->right = MakeList(exprs);
	return outer;
}

SlangObj* SlangParser::Parse(){
	gDebugParser = this;
	nameDict = gDefaultNameDict;
	currentName = GLOBAL_SYMBOL_COUNT;
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
		PrintErrors(*this);
		return nullptr;
	}
	return WrapProgram(exprs);
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
			os << "(lambda (";
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
				os << "#t";
			} else {
				os << "#f";
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
