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
#include <cstring>
#include <iostream>

#define SMALL_SET_SIZE 65536
#define FORWARD_MASK (~7ULL)
#define DICT_UNOCCUPIED_VAL UINT64_MAX
#define SLANG_ENV_BLOCK_SIZE 4
#define SL_ARR_LEN(x) (sizeof(x)/sizeof(x[0]))

#define SLANG_VERSION "0.1.0"

namespace slang {
	extern size_t gNameCollisions;
#ifndef NDEBUG
	extern size_t gMaxArgHeight;
#endif
	typedef uint64_t SymbolName;
	typedef uint64_t ModuleName;
	typedef std::unordered_map<std::string,ModuleName> ModuleNameDict;
	
	inline size_t QuantizeToPowerOfTwo(uint64_t v){
		return (1ULL << (64-__builtin_clzll(v-1)));
	}
	
	inline uint64_t rotleft(uint64_t v,uint64_t s){
		return (v<<s) | (v>>(64-s));
	}
	
	inline uint64_t rotright(uint64_t v,uint64_t s){
		return (v>>s) | (v<<(64-s));
	}
	
	template <typename T>
	struct Vector {
		T* data;
		size_t size;
		size_t cap;
		
		Vector(){
			data = nullptr;
			size = 0;
			cap = 0;
		}
		
		~Vector(){
			if (data){
				free(data);
			}
		}
		
		Vector(const Vector<T>& v){
			data = (T*)malloc(v.size*sizeof(T));
			memcpy(data,v.data,v.size*sizeof(T));
			cap = v.size;
			size = v.size;
		}
		
		Vector(Vector<T>&& v){
			data = v.data;
			size = v.size;
			cap = v.cap;
			v.data = nullptr;
		}
		
		Vector<T>& operator=(const Vector<T>& v){
			data = (T*)malloc(v.size*sizeof(T));
			memcpy(data,v.data,v.size*sizeof(T));
			cap = v.size;
			size = v.size;
			return *this;
		}
		
		Vector<T>& operator=(Vector<T>&& v){
			data = v.data;
			size = v.size;
			cap = v.cap;
			v.data = nullptr;
			return *this;
		}
		
		inline void Reserve(size_t d){
			if (cap>d) return;
			cap = d;
			data = (T*)realloc(data,cap*sizeof(T));
		}
		
		inline void Resize(size_t d){
			if (d<=cap){
				size = d;
				return;
			}
			
			cap = d;
			data = (T*)realloc(data,cap*sizeof(T));
		}
		
		inline void AddSize(size_t s){
			if (size+s<=cap){
				size += s;
				return;
			}
			
			cap = (size+s)*3/2;
			size += s;
			data = (T*)realloc(data,cap*sizeof(T));
		}
		
		inline void Clear(){
			size = 0;
		}
		
		inline void PushBack(const T& val){
			if (size==cap){
				Resize(cap*3/2+1);
			}
			data[size++] = val;
		}
		
		inline T& PlaceBack(){
			if (size==cap){
				Resize(cap*3/2+1);
			}
			
			return data[size++];
		}
		
		inline void PopBack(){
			--size;
		}
		
		inline void PopFrom(size_t index){
			for (size_t i=index;i<size;++i){
				data[i] = data[i+1];
			}
			--size;
		}
		
		inline T& Front() const {
			return data[0];
		}
		
		inline T& Back() const {
			return data[size-1];
		}
		
		inline bool Empty() const {
			return size==0;
		}
	};
	
	inline uint64_t HashSymbolNameFromSV(std::string_view sv){
		uint64_t hash = 0;
		
		for (size_t i=0;i<sv.size();++i){
			hash = rotleft(hash,1);
			hash ^= sv[i];
		}
		hash ^= sv.size();
		return hash;
	}
	
	struct StringLocation {
		size_t start;
		size_t count;
	};
	
	struct SymbolNamePair {
		StringLocation loc;
		SymbolName symbol;
	};
	
	inline std::string_view StringViewFromLocation(const Vector<uint8_t>& arr,StringLocation loc){
		const char* arrStr = (const char*)(arr.data+loc.start);
		return {arrStr,loc.count};
	}
	
	inline uint64_t HashSymbolNameFromLoc(const Vector<uint8_t>& arr,StringLocation loc){
		std::string_view view = StringViewFromLocation(arr,loc);
		return HashSymbolNameFromSV(view);
	}
	
	inline bool StringLocAndViewMatch(const Vector<uint8_t>& arr,StringLocation loc,std::string_view sv){
		if (loc.count!=sv.size())
			return false;
		const char* arrStr = (const char*)(arr.data+loc.start);
		for (size_t i=0;i<loc.count;++i){
			if (arrStr[i]!=sv[i])
				return false;
		}
		return true;
	}
	
	struct SymbolNameDict {
		size_t size;
		size_t cap;
		SymbolNamePair* data;
		const Vector<uint8_t>& strArray;
		
		SymbolNameDict(const Vector<uint8_t>& arr) : strArray(arr){
			size = 0;
			cap = 0;
			data = nullptr;
		}
		
		~SymbolNameDict(){
			if (data){
				free(data);
			}
		}
		
		SymbolNameDict(const SymbolNameDict&) = delete;
		SymbolNameDict(SymbolNameDict&&) = delete;
		SymbolNameDict& operator=(const SymbolNameDict&) = delete;
		SymbolNameDict& operator=(SymbolNameDict&&) = delete;
		
		inline void Reserve(size_t s){
			assert(size==0);
			size_t realSize = QuantizeToPowerOfTwo(s);
			data = (SymbolNamePair*)malloc(sizeof(SymbolNamePair)*realSize);
			memset(data,0xFF,sizeof(SymbolNamePair)*realSize);
			cap = realSize;
		}
		
		inline SymbolName Find(std::string_view sv) const {
			uint64_t hash = HashSymbolNameFromSV(sv);
			
			const SymbolNamePair* end = data+cap;
			const SymbolNamePair* it = data+(hash & (cap-1));
			
			while (true){
				if (it->loc.start==DICT_UNOCCUPIED_VAL)
					return -1ULL;
					
				if (StringLocAndViewMatch(strArray,it->loc,sv)){
					return it->symbol;
				}
				
				++it;
				if (it==end)
					it = data;
			}
		}
		
		inline void DoubleTable(){
			size_t newCap = cap*2;
			SymbolNamePair* oldData = data;
			data = (SymbolNamePair*)malloc(newCap*sizeof(SymbolNamePair));
			memset(data,0xFF,newCap*sizeof(SymbolNamePair));
			
			const SymbolNamePair* it = oldData;
			const SymbolNamePair* end = oldData+cap;
			size = 0;
			cap = newCap;
			gNameCollisions = 0;
			
			while (it!=end){
				if (it->loc.start!=DICT_UNOCCUPIED_VAL)
					Insert(it->loc,it->symbol);
				
				++it;
			}
			
			free(oldData);
		}
		
		inline void Insert(StringLocation loc,SymbolName sym){
			uint64_t hash = HashSymbolNameFromLoc(strArray,loc);
			
			const SymbolNamePair* end = data+cap;
			SymbolNamePair* it = data+(hash & (cap-1));
			
			if (it->loc.start!=DICT_UNOCCUPIED_VAL)
				++gNameCollisions;
			
			while (true){
				if (it->loc.start==DICT_UNOCCUPIED_VAL){
					it->loc = loc;
					it->symbol = sym;
					++size;
					break;
				}
				
				++it;
				if (it==end)
					it = data;
			}
			
			if (size*2>cap){
				DoubleTable();
			}
		}
	};
	
	struct MemLink {
		uint8_t* data;
		uint8_t* write;
		size_t size;
	};
	
	struct MemChain {
		Vector<MemLink> links;
		MemLink* currLink = nullptr;
		size_t startSize;
		
		MemChain(size_t initSize){
			startSize = initSize;
			links.Reserve(8);
			MemLink& first = links.PlaceBack();
			first.size = initSize;
			first.data = (uint8_t*)malloc(initSize);
			first.write = first.data;
			currLink = &first;
		}
		
		~MemChain(){
			for (size_t i=0;i<links.size;++i){
				free(links.data[i].data);
			}
		}
		
		MemChain(const MemChain&) = delete;
		MemChain(MemChain&&) = delete;
		MemChain& operator=(const MemChain&) = delete;
		MemChain& operator=(MemChain&&) = delete;
		
		inline void* Allocate(size_t size){
			if (currLink->write+size<=currLink->data+currLink->size){
				void* d = currLink->write;
				currLink->write += size;
				return d;
			}
			
			size_t newSize = currLink->size*2;
			if (newSize<size*2){
				newSize = size*2;
			}
			MemLink& newLink = links.PlaceBack();
			newLink.data = (uint8_t*)malloc(newSize);
			newLink.size = newSize;
			newLink.write = newLink.data+size;
			currLink = &newLink;
			return newLink.data;
		}
		
		void Reset(){
			size_t endSize = currLink->size;
			for (size_t i=1;i<links.size;++i){
				free(links.data[i].data);
			}
			links.size = 1;
			size_t newSize = endSize;
			
			currLink = links.data;
			if (newSize!=currLink->size)
				currLink->data = (uint8_t*)realloc(currLink->data,newSize);
			currLink->size = newSize;
			currLink->write = currLink->data;
		}
	};
	
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
		Dict,
		InputStream,
		OutputStream,
		EndOfFile,
		Maybe,
		// inaccessible
		Env,
		Storage,
		DictTable,
	};
	
	enum SlangFlag {
		FLAG_FORWARDED =          0b1,
		FLAG_VARIADIC =        0b1000,
		FLAG_MAYBE_OCCUPIED = 0b10000,
		FLAG_CLOSURE =       0b100000,
		FLAG_EXTERNAL =     0b1000000,
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
					uint8_t dictScale;
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
	
	struct SlangDictElement {
		uint64_t hash;
		SlangHeader* key;
		SlangHeader* val;
	};
	
	struct SlangStorage {
		SlangHeader header;
		size_t size;
		size_t capacity;
		union {
			struct {
				uint8_t data[];
			};
			struct {
				SlangHeader* objs[];
			};
			struct {
				SlangDictElement elements[];
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
	
	struct SlangList {
		SlangHeader header;
		SlangHeader* left;
		SlangHeader* right;
	};
	
	uint64_t SlangHashObj(const SlangHeader* obj);
	bool EqualObjs(const SlangHeader* a,const SlangHeader* b);
	
	struct SlangDictTable {
		SlangHeader header;
		size_t size;
		size_t capacity;
		size_t elementOffsets[];
	};
	
	struct SlangDict {
		SlangHeader header;
		SlangDictTable* table;
		SlangStorage* storage;
		
		inline bool LookupKey(SlangHeader* key,SlangHeader** val) const {
			// if storage exists, table will exist
			if (!storage||storage->size==0) return false;
			
			uint64_t hashVal = SlangHashObj(key);
			//std::cout << *key << ',' << hashVal << '\n';
			const size_t* offset = table->elementOffsets+(hashVal & (table->capacity-1));
			const size_t* end = table->elementOffsets+table->capacity;
			SlangDictElement* obj;
			
			while (true){
				if ((uint64_t)*offset == DICT_UNOCCUPIED_VAL){
					return false;
				}
					
				obj = &storage->elements[*offset];
				if (obj->hash==hashVal && EqualObjs(key,obj->key)){
					*val = obj->val;
					return true;
				}
				
				++offset;
				if (offset==end)
					offset = table->elementOffsets;
			}
			
			return true;
		}
		
		inline bool LookupKeyWithHash(
				SlangHeader* key,
				uint64_t hash,
				SlangHeader** val) const {
			// if storage exists, table will exist
			if (!storage||storage->size==0) return false;
			
			const size_t* offset = table->elementOffsets+(hash & (table->capacity-1));
			const size_t* end = table->elementOffsets+table->capacity;
			SlangDictElement* obj;
			
			while (true){
				if ((uint64_t)*offset == DICT_UNOCCUPIED_VAL)
					return false;
					
				obj = &storage->elements[*offset];
				if (obj->hash==hash && EqualObjs(key,obj->key)){
					*val = obj->val;
					return true;
				}
				
				++offset;
				if (offset==end)
					offset = table->elementOffsets;
			}
			
			return true;
		}
		
		inline bool PopKey(SlangHeader* key,SlangHeader** val){
			if (!storage||storage->size==0) return false;
			
			uint64_t hashVal = SlangHashObj(key);
			size_t* offset = table->elementOffsets+(hashVal & (table->capacity-1));
			const size_t* end = table->elementOffsets+table->capacity;
			SlangDictElement* obj;
			
			while (true){
				if ((uint64_t)*offset == DICT_UNOCCUPIED_VAL)
					return false;
					
				obj = &storage->elements[*offset];
				if (obj->hash==hashVal && EqualObjs(key,obj->key)){
					break;
				}
				
				++offset;
				if (offset==end)
					offset = table->elementOffsets;
			}
			
			*val = obj->val;
			if (*offset==storage->size)
				--storage->size;
			else
				memset(obj,0xFF,sizeof(SlangDictElement));
				
			*offset = DICT_UNOCCUPIED_VAL;
			--table->size;
			
			size_t* j = offset;
			const size_t* kDesiredLoc;
			while (true){
				++j;
				if (j==end)
					j = table->elementOffsets;
				
				if ((uint64_t)*j == DICT_UNOCCUPIED_VAL)
					break;
				
				kDesiredLoc = 
					table->elementOffsets +
					(storage->elements[*j].hash & (table->capacity-1));
				
				if (offset<=j){
					if (offset<kDesiredLoc && kDesiredLoc<=j)
						continue;
				} else {
					if (offset<kDesiredLoc || kDesiredLoc<=j)
						continue;
				}
				
				*offset = *j;
				*j = DICT_UNOCCUPIED_VAL;
				
				offset = j;
			}
			
			return true;
		}
		
		inline bool ShouldGrow() const {
			return (table->size*3)/2+1 >= table->capacity;
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
	
	struct SlangMapping {
		SymbolName sym;
		SlangHeader* obj;
	};
	
	struct SlangEnv {
		SlangHeader header;
		SlangEnv* parent;
		SlangEnv* next;
		SlangMapping mappings[SLANG_ENV_BLOCK_SIZE];
		
		inline bool GetSymbol(SymbolName,SlangHeader**) const;
		inline bool HasSymbol(SymbolName) const;
		inline bool DefSymbol(SymbolName,SlangHeader*);
		inline bool SetSymbol(SymbolName,SlangHeader*);
		
		inline SlangHeader* GetIndexed(uint16_t idx) const {
			if (idx>=SLANG_ENV_BLOCK_SIZE)
				return next->GetIndexed(idx-SLANG_ENV_BLOCK_SIZE);
			return mappings[idx].obj;
		}
		
		inline void SetIndexed(uint16_t idx,SlangHeader* obj){
			if (idx>=SLANG_ENV_BLOCK_SIZE)
				return next->SetIndexed(idx-SLANG_ENV_BLOCK_SIZE,obj);
			mappings[idx].obj = obj;
		}
		
		inline void AddBlock(SlangEnv*);
	};
	
	struct ExternalFuncData;
	
	struct SlangLambda {
		SlangHeader header;
		union {
			SlangHeader* expr;
			const ExternalFuncData* extFunc;
			size_t funcIndex;
		};
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
	struct CodeInterpreter;
	
	// max number of args
	#define VARIADIC_ARG_COUNT 65535
	
	/*struct ExternalFunc {
		const char* name;
		SlangFunc func;
		size_t minArgs = 0;
		size_t maxArgs = 0;
	};*/
	
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
			case SlangType::Lambda:
			case SlangType::InputStream:
			case SlangType::OutputStream:
			case SlangType::EndOfFile:
			case SlangType::Maybe:
			case SlangType::Bool:
			case SlangType::Vector:
			case SlangType::String:
			case SlangType::Dict:
			case SlangType::Storage:
			case SlangType::DictTable:
				return true;
			case SlangType::List:
			case SlangType::Symbol:
				return false;
		}
		return false;
	}
	
	inline bool IsNumeric(const SlangHeader* o){
		SlangType t = GetType(o);
		return t==SlangType::Int || t==SlangType::Real;
	}
	
	inline bool IsStream(const SlangHeader* o){
		SlangType t = GetType(o);
		return t==SlangType::InputStream || t==SlangType::OutputStream;
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
	
	typedef void*(*AllocFunc)(void*,size_t);
	struct SlangAllocator {
		void* user;
		AllocFunc alloc;
		
		inline uint8_t* Allocate(size_t);
		inline SlangObj* AllocateObj(SlangType);
		inline SlangLambda* AllocateLambda();
		inline SlangList* AllocateList();
		inline SlangEnv* AllocateEnv();
		inline SlangStorage* AllocateStorage(size_t size,uint16_t elemSize);
		inline SlangDict* AllocateDict();
		inline SlangStorage* AllocateDictStorage(size_t size);
		inline SlangDictTable* AllocateDictTable(size_t size);
		inline SlangVec* AllocateVec(size_t);
		inline SlangStr* AllocateStr(size_t);
		inline SlangStream* AllocateStream(SlangType);
		
		inline SlangObj* MakeInt(int64_t);
		inline SlangObj* MakeReal(double);
		inline SlangObj* MakeSymbol(SymbolName);
		inline SlangHeader* MakeBool(bool);
		inline SlangHeader* MakeEOF();
	};
	
	typedef std::string_view::const_iterator StringIt;
	
	struct LocationData {
		uint32_t line,col;
		uint32_t moduleName;
	};
	
	struct ErrorData {
		LocationData loc;
		std::string type;
		std::string message;
	};
	
	enum class SlangTokenType : uint8_t {
		Symbol = 0,
		Int,
		Real,
		String,
		VectorMarker,
		LeftBracket,
		RightBracket,
		Quote,
		Quasiquote,
		Unquote,
		UnquoteSplicing,
		Not,
		Negation,
		Invert,
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
		
		inline void TokenizeIdentifier(SlangToken&);
		inline void TokenizeNumber(SlangToken&);
		SlangToken NextToken();
	};
	
	struct SlangParser {
		std::unique_ptr<SlangTokenizer> tokenizer;
		SlangToken token;
		
		Vector<uint8_t> nameStringStorage;
		SymbolNameDict nameDict;
		Vector<StringLocation> symbolToStringArray;
		SymbolName currentName;
		
		std::vector<ErrorData> errors;
		
		uint32_t currModule;
		
		SlangAllocator alloc;
		size_t maxCodeSize;
		MemChain memChain;
		
		std::unordered_map<const SlangHeader*,LocationData> codeMap;
		
		SlangParser();
		
		SlangParser(const SlangParser&) = delete;
		SlangParser& operator=(const SlangParser&) = delete;
		SlangParser(SlangParser&&) = delete;
		SlangParser& operator=(SlangParser&&) = delete;
		
		inline SlangList* WrapExprIn(SymbolName func,SlangHeader* expr);
		inline SlangList* WrapExprSplice(SymbolName func,SlangList* expr);
		inline LocationData GetExprLocation(const SlangHeader*);
		
		std::string_view GetSymbolString(SymbolName name) const;
		SymbolName RegisterSymbol(std::string_view name);
		
		void PushError(const std::string& msg);
		
		inline void NextToken();
		bool ParseLine(SlangHeader**);
		bool ParseExpr(SlangHeader**);
		bool ParseObj(SlangHeader**);
		bool ParseVec(SlangHeader**);
		void TestSeqMacro(SlangList* list,size_t argCount);
		
		void SetCodeString(std::string_view code,ModuleName name);
	};
	
	struct CodeLocationPair {
		uint32_t codeIndex;
		uint32_t line;
		uint32_t col;
	};
	
	typedef Vector<SymbolName> ParamData;
	struct CodeBlock {
		size_t size;
		SymbolName name;
		ParamData params;
		ParamData defs;
		Vector<CodeLocationPair> locData;
		uint32_t moduleIndex;
		uint8_t isVariadic;
		uint8_t isClosure;
		uint8_t isPure;
		uint8_t* start;
		uint8_t* write;
	};
	
	struct LambdaData {
		SymbolName funcName;
        ParamData params;
		ParamData defs;
		Vector<size_t> paramHeights;
		size_t defHeightsStart = 0;
		bool isVariadic = false;
		bool isClosure = false;
		bool isStar = false;
		bool isLetRec = false;
		uint16_t currLetInit = 0;
		uint32_t currInternalDef = 0;
	};
	
	struct CaseDictElement {
		const SlangHeader* key;
		size_t offset;
	};
	
	struct CaseDict {
		size_t capacity;
		size_t elemsStart;
		size_t elseOffset;
	};
	
	struct CodeWriter {
		SlangParser& parser;
		SlangAllocator alloc;
		MemChain memChain;
		
		std::vector<LambdaData> lambdaStack;
		
		std::vector<CodeBlock> lambdaCodes;
		std::vector<std::map<SymbolName,size_t>> knownLambdaStack;
		Vector<size_t> currHeights;
		Vector<size_t> currFrames;
		
		Vector<CaseDict> caseDicts;
		Vector<CaseDictElement> caseDictElements;
		
		CodeBlock* curr;
		uint8_t* lastInst;
		SymbolName currDefName;
		SymbolName currSetName;
		
		uint32_t currModuleIndex;
		
		SlangHeader* constTrueObj;
		SlangHeader* constFalseObj;
		SlangHeader* constZeroObj;
		SlangHeader* constOneObj;
		SlangHeader* constEOFObj;
		SlangHeader* constElseObj;
		
		bool optimize;
		
		size_t totalAlloc;
		size_t evalFuncIndex = -1ULL;
		
		std::vector<ErrorData> errors;
		
		SlangHeader* Copy(const SlangHeader* obj);
		size_t AllocNewBlock(size_t initSize);
		void ReallocCurrBlock(size_t newSize);
		void WriteOpCode(uint8_t op);
		void WritePointer(const void* ptr);
		void WriteInt16(uint16_t i);
		void WriteSInt32(int32_t i);
		void WriteInt32(uint32_t i);
		void WriteInt64(uint64_t i);
		void WriteSymbolName(SymbolName sym);
		size_t WriteStartJump();
		void FinishJump(size_t);
		size_t WriteStartCJump(bool jumpOn);
		size_t WriteStartCJumpPop(bool jumpOn);
		void FinishCJump(size_t);
		
		void WritePushFrame();
		void WriteCall(bool terminating=false);
		void WriteCallSym(SymbolName sym,bool terminating=false);
		void WriteRet();
		void WriteUnpack();
		void WritePop();
		void WriteNull();
		void WriteTrue();
		void WriteFalse();
		void WriteZero();
		void WriteOne();
		void WriteLoadPtr(const void* ptr);
		void WriteMakeLambda(uint64_t);
		void WriteMakePair();
		void WriteMakeVec();
		
		size_t StartTryOp();
		void FinishTryOp(size_t);
		
		bool SymbolInStarParams(SymbolName,uint32_t&) const;
		bool SymbolInInternalDefs(SymbolName,uint32_t&) const;
		bool SymbolInParams(SymbolName,uint16_t&) const;
		bool SymbolNotInAnyParams(SymbolName) const;
		bool ParamIsInited(uint16_t) const;
		void AddClosureParam(SymbolName);
		
		inline bool IsFuncPure(SymbolName) const;
		inline bool IsLambdaPure(const SlangList*) const;
		inline bool IsPure(const SlangHeader*) const;
		inline bool KnownLambdaArityCheck(const SlangHeader* expr,SymbolName funcName,size_t argCount);
		
		CodeWriter(SlangParser&);
		~CodeWriter();
		CodeWriter(const CodeWriter&) = delete;
		CodeWriter& operator=(const CodeWriter&) = delete;
		CodeWriter(CodeWriter&&) = delete;
		CodeWriter& operator=(CodeWriter&&) = delete;
		
		void CaseDictAlloc(CaseDict& dict,size_t elems);
		bool CaseDictSet(const CaseDict& dict,const SlangHeader* key,size_t offset);
		inline size_t CaseDictGet(const CaseDict& dict,const SlangHeader* key) const;
		
		SlangHeader* MakeIntConst(int64_t i);
		SlangHeader* MakeSymbolConst(SymbolName sym);
		
		void InitCompile();
		
		bool CompileData(const SlangHeader* obj);
		bool CompileCode(const std::string& code);
		bool CompileModule(ModuleName name,const std::string& code,size_t& funcIndex);
		
		bool CompileExpr(const SlangHeader*,bool terminating=false);
		bool CompileConst(const SlangHeader*);
		bool CompileLambdaBody(
			const SlangList* exprs,
			const std::vector<SlangHeader*>* initExprs,
			const std::vector<SlangHeader*>* internalDefs,
			size_t& lambdaIndex
		);
		
		bool CompileDefine(const SlangHeader*);
		bool CompileLambda(const SlangHeader*);
		bool CompileDo(const SlangHeader*,bool terminating=false);
		bool CompileLet(const SlangHeader*,bool terminating=false);
		bool CompileLetRec(const SlangHeader*,bool terminating=false);
		bool CompileIf(const SlangHeader*,bool terminating=false);
		bool CompileCond(const SlangHeader*,bool terminating=false);
		bool CompileCase(const SlangHeader*,bool terminating=false);
		bool CompileApply(const SlangHeader*,bool terminating=false);
		bool CompileAnd(const SlangHeader*,bool terminating=false);
		bool CompileOr(const SlangHeader*,bool terminating=false);
		bool CompileMap(const SlangHeader*);
		bool CompileForeach(const SlangHeader*);
		bool CompileFilter(const SlangHeader*);
		bool CompileFold(const SlangHeader*);
		bool CompileTry(const SlangHeader*);
		bool CompileUnwrap(const SlangHeader*);
		bool CompileQuote(const SlangHeader*);
		bool CompileUnquote(const SlangHeader*);
		bool CompileUnquoteSplicing(const SlangHeader*);
		bool QuasiquoteList(const SlangList*,size_t);
		bool QuasiquoteVector(const SlangVec*,size_t);
		bool CompileQuasiquote(const SlangHeader*);
		bool CompileNot(const SlangHeader*);
		bool CompileInc(const SlangHeader*);
		bool CompileDec(const SlangHeader*);
		bool CompileAdd(const SlangHeader*);
		bool CompileSub(const SlangHeader*);
		bool CompileMul(const SlangHeader*);
		bool CompileDiv(const SlangHeader*);
		bool CompileEq(const SlangHeader*);
		bool CompilePair(const SlangHeader*);
		bool CompileLeft(const SlangHeader*);
		bool CompileRight(const SlangHeader*);
		bool CompileSetLeft(const SlangHeader*);
		bool CompileSetRight(const SlangHeader*);
		bool CompileVec(const SlangHeader*);
		bool CompileIsPure(const SlangHeader*);
		bool CompileExport(const SlangHeader*);
		bool CompileImport(const SlangHeader*);
		
		void Optimize(CodeBlock& block);
		
		void Reset();
		void MakeDefaultObjs();
		
		void AddCodeLocation(const SlangHeader* expr);
		LocationData FindCodeLocation(size_t funcIndex,size_t offset) const;
		size_t GetCodeLocationsSize() const;
		
		void PushError(const SlangHeader*,const std::string&,const std::string&);
		void TypeError(const SlangHeader*,SlangType found,SlangType expected);
		void DefError(const SlangHeader*);
		void ReservedError(const SlangHeader*,SymbolName);
		void RedefinedError(const SlangHeader*,SymbolName);
		void FuncRedefinedError(const SlangHeader*,SymbolName);
		void LetRecError(const SlangHeader*,SymbolName);
		void ArityError(const SlangHeader* head,size_t found,size_t expectMin,size_t expectMax);
		void DotError(const SlangHeader*);
	};
	
	struct StackData {
		size_t base;
	};
	
	struct FuncData {
		size_t funcIndex;
		size_t argsFrame;
		SlangEnv* env;
		SlangEnv* globalEnv;
		const uint8_t* retAddr;
	};
	
	struct TryData {
		const uint8_t* gotoAddr;
		size_t stackSize;
		size_t argStackSize;
		size_t funcStackSize;
		size_t globalStackSize;
	};
	
	struct ModuleData {
		SlangEnv* globalEnv;
		SlangEnv* exportEnv;
	};
	
	typedef void(*FinalizerFunc)(CodeInterpreter* s,SlangHeader* obj);
	struct Finalizer {
		SlangHeader* obj;
		FinalizerFunc func;
	};
	
	struct CodeInterpreter {
		SlangParser parser;
		CodeWriter codeWriter;
		MemArena* arena;
		SlangAllocator alloc;
		
		MemChain memChain;
		SlangAllocator constAlloc;
		MemChain evalMemChain;
		SlangAllocator evalAlloc;
		
		bool halted;
		size_t stepCount;
		const uint8_t* pc;
		Vector<StackData> stack;
		Vector<SlangHeader*> argStack;
		Vector<FuncData> funcStack;
		Vector<TryData> tryStack;
		
		Vector<ModuleData> modules;
		ModuleName currModuleName;
		ModuleNameDict moduleNameDict;
		
		std::unordered_map<SymbolName,SlangEnv*> builtinModulesMap;
		
		std::vector<ErrorData> errors;
		Vector<Finalizer> finalizers;
		SlangEnv* lamEnv;
		
		inline ModuleName RegisterModuleName(const std::string& name){
			if (moduleNameDict.contains(name)){
				return moduleNameDict.at(name);
			}
			
			ModuleName mn = currModuleName++;
			moduleNameDict[name] = mn;
			return mn;
		}
		
		inline std::string GetModuleString(ModuleName name) const {
			for (auto& [k,v] : moduleNameDict){
				if (name==v)
					return k;
			}
			return "INVALID MODULE";
		}
		
		inline void InternalDef(size_t index,SlangHeader* obj){
			SlangEnv* env = GetCurrEnv();
			SymbolName sym = codeWriter.lambdaCodes[funcStack.Back().funcIndex].defs.data[index];
			DefOrSetEnvSymbol(env,sym,obj);
		}
		
		inline SlangHeader* GetArg(size_t i) const {
			return argStack.data[stack.Back().base+i];
		}
		
		inline void SetArg(size_t i,SlangHeader* arg){
			argStack.data[stack.Back().base+i] = arg;
		}
		
		inline size_t GetArgCount() const {
			return argStack.size-stack.Back().base;
		}
		
		inline void PushArg(SlangHeader* arg){
			if (argStack.size>=argStack.cap){
				argStack.data = (SlangHeader**)realloc(argStack.data,argStack.cap*2*sizeof(SlangHeader*));
				argStack.cap *= 2;
			}
			argStack.data[argStack.size++] = arg;
#ifndef NDEBUG
			if (argStack.size>gMaxArgHeight)
				gMaxArgHeight = argStack.size;
#endif
		}
		
		inline SlangHeader* PopArg(){
			assert(!argStack.Empty());
			SlangHeader* e = argStack.Back();
			argStack.PopBack();
			return e;
		}
		
		inline SlangHeader* PeekArg(){
			assert(!argStack.Empty());
			SlangHeader* e = argStack.Back();
			return e;
		}
		
		inline SlangHeader* GetLocalArg(uint16_t index){
			size_t base = stack.data[funcStack.Back().argsFrame].base;
			return argStack.data[base+index];
		}
		
		inline void PushLocal(uint16_t index){
			size_t base = stack.data[funcStack.Back().argsFrame].base;
			if (argStack.size>=argStack.cap){
				argStack.data = (SlangHeader**)realloc(argStack.data,argStack.cap*2*sizeof(SlangHeader*));
				argStack.cap *= 2;
			}
			argStack.data[argStack.size++] = argStack.data[base+index];
		}
		
		inline void PushRecLocal(uint32_t index){
			size_t base = argStack.size-index;
			if (argStack.size>=argStack.cap){
				argStack.data = (SlangHeader**)realloc(argStack.data,argStack.cap*2*sizeof(SlangHeader*));
				argStack.cap *= 2;
			}
			argStack.data[argStack.size++] = argStack.data[base];
		}
		
		inline void SetLocalArg(uint16_t index,SlangHeader* val){
			size_t argsFrame = funcStack.Back().argsFrame;
			SlangEnv* e = funcStack.Back().env;
			size_t base = stack.data[argsFrame].base;
			argStack.data[base+index] = val;
			if (codeWriter.lambdaCodes[funcStack.Back().funcIndex].isClosure)
				e->SetIndexed(index,val);
		}
		
		inline void SetRecLocalArg(uint32_t index,SlangHeader* val){
			argStack.data[argStack.size-index+1] = val;
		}
		
		inline void PushFrame(){
			if (stack.size>=stack.cap){
				stack.data = (StackData*)realloc(stack.data,stack.cap*2*sizeof(StackData));
				stack.cap *= 2;
			}
			StackData& d = stack.PlaceBack();
			d.base = argStack.size;
		}
		
		inline void PopFrame(){
			assert(stack.size>1);
			assert(argStack.size>=stack.Back().base);
			argStack.size = stack.Back().base;
			stack.PopBack();
		}
		
		inline SlangEnv* GetCurrEnv() const {
			return funcStack.Back().env;
		}
		
		inline void PushTry();
		inline void LoadTry();
		
		inline void Call(size_t funcIndex,SlangEnv* env);
		inline void RetCall(size_t funcIndex,SlangEnv* env);
		inline void Recurse();
		inline void Return(SlangHeader* val);
		
		inline void RetCallBuiltin(){
			StackData& d = stack.Back();
			FuncData& f = funcStack.Back();
			size_t argCount = GetArgCount();
			size_t lowerBase = stack.data[f.argsFrame].base;
			size_t upperBase = d.base;
			// copy args down
			for (size_t i=0;i<argCount;++i){
				argStack.data[lowerBase+i] = argStack.data[upperBase+i];
			}
			// manual pop frame
			stack.PopBack();
			argStack.size = lowerBase+argCount;
		}
		
		size_t GetPCOffset() const;
		
		inline SlangEnv* AllocateEnvs(size_t);
		inline void DefEnvSymbol(SlangEnv* e,SymbolName name,SlangHeader* val);
		void DefOrSetEnvSymbol(SlangEnv* e,SymbolName name,SlangHeader* val);
		inline void DefCurrEnvSymbol(SymbolName name,SlangHeader* val);
		inline bool SetRecursiveSymbol(SymbolName name,SlangHeader* val);
		inline SlangLambda* CreateLambda(size_t index);
		inline SlangList* MakeVariadicList(size_t start,size_t end);
		inline SlangVec* MakeVecFromArgs(size_t start,size_t end);
		SlangHeader* Copy(SlangHeader*);
		inline SlangList* CopyList(SlangList*);
		inline void RehashDict(SlangDict* dict);
		inline SlangDict* ReallocDict(SlangDict* dict);
		inline void RawDictInsertStorage(SlangDict* dict,uint64_t hash,SlangHeader* key,SlangHeader* val);
		inline void RawDictInsert(SlangDict* dict,SlangHeader* key,SlangHeader* val);
		inline void DictInsert(SlangDict* dict,SlangHeader* key,SlangHeader* val);
		inline SlangStr* ReallocateStr(SlangStr*,size_t);
		inline SlangStream* ReallocateStream(SlangStream*,size_t);
		inline SlangStream* MakeStringInputStream(SlangStr* str);
		inline SlangStream* MakeStringOutputStream(SlangStr* str);
		inline void ImportEnv(const SlangEnv* env);
		
		void SetGlobalSymbol(const std::string& name,SlangHeader* val);
		bool ParseSlangString(const SlangStr&,SlangHeader**);
		
		void InitBuiltinModules();
		bool ImportBuiltinModule(SymbolName name);
		
		inline void AddFinalizer(const Finalizer&);
		inline void RemoveFinalizer(SlangHeader*);
		
		inline bool SlangOutputToFile(FILE* file,SlangHeader* obj);
		inline SlangHeader* SlangInputFromFile(FILE* file);
		inline bool SlangOutputToString(SlangStream* stream,SlangHeader* obj);
		inline SlangHeader* SlangInputFromString(SlangStream* stream);
		
		inline void ReallocSet(size_t newSize);
		inline void SmallGC(size_t);
		
		CodeInterpreter();
		~CodeInterpreter();
		
		CodeInterpreter(const CodeInterpreter&) = delete;
		CodeInterpreter& operator=(const CodeInterpreter&) = delete;
		CodeInterpreter(CodeInterpreter&&) = delete;
		CodeInterpreter& operator=(CodeInterpreter&&) = delete;
		
		SlangHeader* Parse(const std::string& prog);
		bool CompileInto(SlangHeader* prog,size_t);
		bool Compile(SlangHeader* prog,size_t&);
		inline bool InlineStep();
		bool Step();
		
		void ResetState();
		void SetupDefaultModule(const std::string& name);
		bool LoadProgram(const std::string& filename,const std::string& code);
		bool LoadModule(ModuleName name,const std::string& code,size_t& funcIndex);
		bool LoadExpr(const std::string& code);
		bool Run();
		
		bool EvalCall(SlangHeader*);
		
		void DisplayErrors() const;
		
		void PushError(const std::string&,const std::string&);
		void EvalError();
		void UndefinedError(SymbolName);
		void RedefinedError(SymbolName);
		void TypeError(SlangType found,SlangType expected);
		void TypeError2(SlangType found,SlangType expected1,SlangType expected2);
		void CharTypeError();
		void AssertError();
		void UnwrapError();
		void ArityError(size_t found,size_t expectMin,size_t expectMax);
		void IndexError(ssize_t desired,size_t len);
		void ListIndexError(ssize_t desired);
		void FileError(const std::string&);
		void StreamError(const std::string&);
		void ExportError(SymbolName sym);
		void DotError();
		void ZeroDivisionError();
	};
	
	size_t PrintCode(const uint8_t* code,const uint8_t* end,std::ostream&,const uint8_t* pc=nullptr);
	void PrintErrors(const std::vector<ErrorData>& errors);
	
	std::ostream& operator<<(std::ostream&,const SlangHeader&);
}
