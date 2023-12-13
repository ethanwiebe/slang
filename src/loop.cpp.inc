
inline void LOOP_NAME(CodeInterpreter* c){
	size_t stepCount = 0;
	SlangHeader* const trueObj = c->codeWriter.constTrueObj;
	SlangHeader* const falseObj = c->codeWriter.constFalseObj;
	SlangHeader* const zeroObj = c->codeWriter.constZeroObj;
	SlangHeader* const oneObj = c->codeWriter.constOneObj;
	uint8_t op;
	size_t i;
	bool boolVal;
	uint16_t localIdx;
	int32_t jumpVal;
	uint32_t v32;
	uint64_t v64;
	uint64_t v642;
	int64_t i64;
	SlangHeader* val;
	SlangHeader* val2;
	SlangList* list;
	SlangEnv* env;
	SlangEnv* newEnv;
	SlangLambda* lam;
	const CodeBlock* block;
	SymbolName sym;
	
	LOOP_PREAMBLE()
		LOOP_INST(SLANG_OP_NOOP)
			NEXT_INST();
		LOOP_INST(SLANG_OP_HALT)
			if (c->funcStack.size>1){
				v64 = c->codeWriter.lambdaCodes[c->funcStack.Back().funcIndex].moduleIndex;
				env = c->modules.data[v64].exportEnv;
				
				v64 = c->stack.Back().base;
				c->pc = c->funcStack.Back().retAddr-SlangOpSizes[SLANG_OP_HALT];
				c->argStack.data[v64] = c->argStack.data[c->argStack.size-1];
				c->argStack.size = v64+1;
				c->stack.PopBack();
				c->funcStack.PopBack();
				c->ImportEnv(env);
				NEXT_INST();
			}
			c->stepCount = stepCount;
			c->halted = true;
			return;
		LOOP_INST(SLANG_OP_NULL)
			c->PushArg(nullptr);
			NEXT_INST();
		LOOP_INST(SLANG_OP_LOAD_PTR)
			v64 = *(uint64_t*)(c->pc+OPCODE_SIZE);
			c->PushArg((SlangHeader*)v64);
			NEXT_INST();
		LOOP_INST(SLANG_OP_BOOL_TRUE)
			c->PushArg(trueObj);
			NEXT_INST();
		LOOP_INST(SLANG_OP_BOOL_FALSE)
			c->PushArg(falseObj);
			NEXT_INST();
		LOOP_INST(SLANG_OP_ZERO)
			c->PushArg(zeroObj);
			NEXT_INST();
		LOOP_INST(SLANG_OP_ONE)
			c->PushArg(oneObj);
			NEXT_INST();
		LOOP_INST(SLANG_OP_PUSH_LAMBDA)
			v64 = *(uint64_t*)(c->pc+OPCODE_SIZE);
			c->PushArg((SlangHeader*)c->CreateLambda(v64));
			NEXT_INST();
		LOOP_INST(SLANG_OP_LOOKUP)
			sym = *(SymbolName*)(c->pc+OPCODE_SIZE);
			if (c->funcStack.Back().isClosure){
				if (!GetRecursiveSymbol(c->GetCurrEnv(),sym,&val)){
					c->UndefinedError(sym);
					return;
				}
			} else {
				v32 = *(uint32_t*)(c->pc+OPCODE_SIZE+sizeof(SymbolName));
				if (!c->GetStackSymbol(c->funcStack.size-v32,sym,&val)){
					c->UndefinedError(sym);
					return;
				}
			}
			c->PushArg(val);
			NEXT_INST();
		LOOP_INST(SLANG_OP_SET)
			sym = *(SymbolName*)(c->pc+OPCODE_SIZE);
			val = c->PopArg();
			if (!c->SetRecursiveSymbol(sym,val)){
				c->DefCurrEnvSymbol(sym,val);
			}
			NEXT_INST();
		LOOP_INST(SLANG_OP_GET_LOCAL)
			localIdx = *(uint16_t*)(c->pc+OPCODE_SIZE);
			c->PushLocal(localIdx);
			NEXT_INST();
		LOOP_INST(SLANG_OP_SET_LOCAL)
			localIdx = *(uint16_t*)(c->pc+OPCODE_SIZE);
			val = c->PopArg();
			c->SetLocalArg(localIdx,val);
			NEXT_INST();
		LOOP_INST(SLANG_OP_GET_GLOBAL)
			sym = *(SymbolName*)(c->pc+OPCODE_SIZE);
			env = c->funcStack.Back().globalEnv;
			if (!env->GetSymbol(sym,&val)){
				c->UndefinedError(sym);
				return;
			}
			c->PushArg(val);
			NEXT_INST();
		LOOP_INST(SLANG_OP_SET_GLOBAL)
			sym = *(SymbolName*)(c->pc+OPCODE_SIZE);
			val = c->PopArg();
			env = c->funcStack.Back().globalEnv;
			if (!env->SetSymbol(sym,val)){
				c->UndefinedError(sym);
				return;
			}
			NEXT_INST();
		LOOP_INST(SLANG_OP_DEF_GLOBAL)
			sym = *(SymbolName*)(c->pc+OPCODE_SIZE);
			val = c->PopArg();
			env = c->funcStack.Back().globalEnv;
			if (!env->SetSymbol(sym,val)){
				if (!env->DefSymbol(sym,val)){
					c->PushArg(val);
					newEnv = c->AllocateEnvs(1);
					newEnv->DefSymbol(sym,c->PopArg());
					c->funcStack.Back().globalEnv->AddBlock(newEnv);
				}
			}
			NEXT_INST();
		LOOP_INST(SLANG_OP_GET_STACK)
			v32 = *(uint32_t*)(c->pc+OPCODE_SIZE);
			c->PushRecLocal(v32);
			NEXT_INST();
		LOOP_INST(SLANG_OP_SET_STACK)
			v32 = *(uint32_t*)(c->pc+OPCODE_SIZE);
			val = c->PopArg();
			c->SetRecLocalArg(v32,val);
			NEXT_INST();
		LOOP_INST(SLANG_OP_PUSH_FRAME)
			c->PushFrame();
#ifndef NDEBUG
			gMaxStackHeight = (c->stack.size>gMaxStackHeight) ? c->stack.size : gMaxStackHeight;
#endif
			NEXT_INST();
		LOOP_INST(SLANG_OP_POP_ARG)
			c->PopArg();
			NEXT_INST();
		LOOP_INST(SLANG_OP_INTERNAL_DEF)
			if (!c->codeWriter.lambdaCodes[c->funcStack.Back().funcIndex].isClosure){
				NEXT_INST();
			}
			
			v32 = *(uint32_t*)(c->pc+OPCODE_SIZE);
			c->InternalDef(v32,c->PeekArg());
			
			NEXT_INST();
		LOOP_INST(SLANG_OP_UNPACK)
			val = c->PopArg();
			if (!IsList(val)){
				c->TypeError(val->type,SlangType::List);
				return;
			}
			while (val){
				c->PushArg(((SlangList*)val)->left);
				
				val = ((SlangList*)val)->right;
				if (!IsList(val)){
					c->TypeError(val->type,SlangType::List);
					return;
				}
			}
			NEXT_INST();
		LOOP_INST(SLANG_OP_COPY)
			val = c->PopArg();
			c->PushArg(c->Copy(val));
			NEXT_INST();
		LOOP_INST(SLANG_OP_CALL)
			val = c->PopArg();
			if (GetType(val)!=SlangType::Lambda){
				if (GetType(val)==SlangType::Symbol&&((SlangObj*)val)->symbol<GLOBAL_SYMBOL_COUNT){
					sym = ((SlangObj*)val)->symbol;
					if (!GoodArity(sym,c->GetArgCount())){
						c->ArityError(
							c->GetArgCount(),
							gGlobalArityArray[sym].min,
							gGlobalArityArray[sym].max
						);
						return;
					}
					if (!CodeBuiltinFuncs[sym](c))
						return;
					NEXT_INST();
				}
				c->TypeError(GetType(val),SlangType::Lambda);
				return;
			} else if (val->flags & FLAG_EXTERNAL){
				if (!GoodExtArity(((SlangLambda*)val)->extFunc,c->GetArgCount())){
					c->ArityError(
						c->GetArgCount(),
						((SlangLambda*)val)->extFunc->minArgs,
						((SlangLambda*)val)->extFunc->maxArgs
					);
					return;
				}
				if (!((SlangLambda*)val)->extFunc->func(c))
					return;
				NEXT_INST();
			}
			
			lam = (SlangLambda*)val;
			boolVal = (val->flags & FLAG_CLOSURE);
			v64 = lam->funcIndex;
			block = &c->codeWriter.lambdaCodes[v64];
			c->lamEnv = lam->env;
			if (!CFHandleArgs(
					c,
					lam->env,
					*block))
				return;
				
			c->Call(v64,c->lamEnv,boolVal);
			c->lamEnv = nullptr;
			NEXT_INST();
		LOOP_INST(SLANG_OP_CALLSYM)
			localIdx = *(uint16_t*)(c->pc+OPCODE_SIZE);
			assert(localIdx<GLOBAL_SYMBOL_COUNT);
			//c->funcStack.Back().retAddr = c->pc+SlangOpSizes[SLANG_OP_CALLSYM];
			if (!CodeBuiltinFuncs[localIdx](c))
				return;
			NEXT_INST();
		LOOP_INST(SLANG_OP_RET)
			v64 = c->stack.Back().base;
			c->pc = c->funcStack.Back().retAddr-SlangOpSizes[SLANG_OP_RET];
			val = c->argStack.data[c->argStack.size-1];
			if (val&&val->type==SlangType::Lambda){
				val->flags |= FLAG_CLOSURE;
			}
			c->argStack.data[v64] = val;
			c->argStack.size = v64+1;
			c->stack.PopBack();
			c->funcStack.PopBack();
			//c->currGlobalEnv = c->funcStack.Back().globalEnv;
			NEXT_INST();
		LOOP_INST(SLANG_OP_RETCALL)
			val = c->PopArg();
			if (GetType(val)!=SlangType::Lambda){
				if (GetType(val)==SlangType::Symbol&&((SlangObj*)val)->symbol<GLOBAL_SYMBOL_COUNT){
					sym = ((SlangObj*)val)->symbol;
					if (!GoodArity(sym,c->GetArgCount())){
						c->ArityError(
							c->GetArgCount(),
							gGlobalArityArray[sym].min,
							gGlobalArityArray[sym].max
						);
						return;
					}
					c->RetCallBuiltin();
					if (!CodeBuiltinFuncs[sym](c))
						return;
					c->pc = c->funcStack.Back().retAddr-SlangOpSizes[SLANG_OP_RETCALL];
					c->funcStack.PopBack();
					NEXT_INST();
				}
				c->TypeError(GetType(val),SlangType::Lambda);
				return;
			} else if (val->flags & FLAG_EXTERNAL){
				if (!GoodExtArity(((SlangLambda*)val)->extFunc,c->GetArgCount())){
					c->ArityError(
						c->GetArgCount(),
						((SlangLambda*)val)->extFunc->minArgs,
						((SlangLambda*)val)->extFunc->maxArgs
					);
					return;
				}
				c->RetCallBuiltin();
				if (!((SlangLambda*)val)->extFunc->func(c))
					return;
				c->pc = c->funcStack.Back().retAddr-SlangOpSizes[SLANG_OP_RETCALL];
				c->funcStack.PopBack();
				NEXT_INST();
			}
			
			lam = (SlangLambda*)val;
			//boolVal = (val->flags&FLAG_CLOSURE);
			v64 = lam->funcIndex;
			block = &c->codeWriter.lambdaCodes[v64];
			c->lamEnv = lam->env;
			if (!CFHandleArgs(c,
								lam->env,
								*block))
				return;
			
			c->RetCall(v64,c->lamEnv,true);
			c->lamEnv = nullptr;
			NEXT_INST();
		LOOP_INST(SLANG_OP_RETCALLSYM)
			localIdx = *(uint16_t*)(c->pc+OPCODE_SIZE);
			assert(localIdx<GLOBAL_SYMBOL_COUNT);
			c->RetCallBuiltin();
			if (!CodeBuiltinFuncs[localIdx](c))
				return;
			c->pc = c->funcStack.Back().retAddr-SlangOpSizes[SLANG_OP_RETCALLSYM];
			c->funcStack.PopBack();
			NEXT_INST();
		LOOP_INST(SLANG_OP_RECURSE)
			v64 = c->funcStack.Back().funcIndex;
			block = &c->codeWriter.lambdaCodes[v64];
			c->lamEnv = c->funcStack.Back().env;
			if (!CFHandleArgs(c,c->lamEnv,*block))
				return;
				
			c->Recurse();
			NEXT_INST();
		LOOP_INST(SLANG_OP_JUMP)
			jumpVal = *(int32_t*)(c->pc+OPCODE_SIZE);
			c->pc += jumpVal-SlangOpSizes[SLANG_OP_JUMP];
			NEXT_INST();
		LOOP_INST(SLANG_OP_CJUMP_POP)
			val = c->PopArg();
			if (!ConvertToBool(val)){
				jumpVal = *(int32_t*)(c->pc+OPCODE_SIZE);
				c->pc += jumpVal-SlangOpSizes[SLANG_OP_CJUMP_POP];
			}
			NEXT_INST();
		LOOP_INST(SLANG_OP_CNJUMP_POP)
			val = c->PopArg();
			if (ConvertToBool(val)){
				jumpVal = *(int32_t*)(c->pc+OPCODE_SIZE);
				c->pc += jumpVal-SlangOpSizes[SLANG_OP_CNJUMP_POP];
			}
			NEXT_INST();
		LOOP_INST(SLANG_OP_CJUMP)
			val = c->PeekArg();
			if (!ConvertToBool(val)){
				jumpVal = *(int32_t*)(c->pc+OPCODE_SIZE);
				c->pc += jumpVal-SlangOpSizes[SLANG_OP_CJUMP];
			}
			NEXT_INST();
		LOOP_INST(SLANG_OP_CNJUMP)
			val = c->PeekArg();
			if (ConvertToBool(val)){
				jumpVal = *(int32_t*)(c->pc+OPCODE_SIZE);
				c->pc += jumpVal-SlangOpSizes[SLANG_OP_CNJUMP];
			}
			NEXT_INST();
		LOOP_INST(SLANG_OP_CASE_JUMP)
			v32 = *(uint32_t*)(c->pc+OPCODE_SIZE);
			val = c->PopArg();
			v64 = c->codeWriter.CaseDictGet(c->codeWriter.caseDicts.data[v32],val);
			c->pc += v64-SlangOpSizes[SLANG_OP_CASE_JUMP];
			
			NEXT_INST();
		LOOP_INST(SLANG_OP_TRY)
			c->PushTry();
			NEXT_INST();
		LOOP_INST(SLANG_OP_MAYBE_NULL)
			val = (SlangHeader*)c->alloc.AllocateObj(SlangType::Maybe);
			((SlangObj*)val)->maybe = nullptr;
			c->PushArg(val);
			NEXT_INST();
		LOOP_INST(SLANG_OP_MAYBE_WRAP)
			val2 = (SlangHeader*)c->alloc.AllocateObj(SlangType::Maybe);
			val = c->PopArg();
			((SlangObj*)val2)->maybe = val;
			val2->flags |= FLAG_MAYBE_OCCUPIED;
			c->PushArg(val2);
			assert(c->tryStack.size);
			--c->tryStack.size;
			NEXT_INST();
		LOOP_INST(SLANG_OP_MAYBE_UNWRAP)
			val = c->PopArg();
			LOOP_TYPE_CHECK_EXACT(val,SlangType::Maybe);
			
			if (!(val->flags & FLAG_MAYBE_OCCUPIED)){
				c->UnwrapError();
				return;
			}
			c->PushArg(((SlangObj*)val)->maybe);
			NEXT_INST();
		LOOP_INST(SLANG_OP_MAP_STEP)
			localIdx = *(uint16_t*)(c->pc+OPCODE_SIZE);
			v64 = c->stack.Back().base;
			
			val = c->argStack.data[v64+localIdx+2];
			if (!val&&c->argStack.size>v64+3+localIdx){
				val = (SlangHeader*)c->alloc.AllocateList();
				((SlangList*)val)->left = c->PopArg();
				c->argStack.data[v64+localIdx+1] = val;
				c->argStack.data[v64+localIdx+2] = val;
			} else if (val){
				val2 = (SlangHeader*)c->alloc.AllocateList();
				((SlangList*)c->argStack.data[v64+localIdx+2])->right = val2;
				val = c->argStack.data[v64+localIdx+2];
				c->argStack.data[v64+localIdx+2] = ((SlangList*)val)->right;
				((SlangList*)c->argStack.data[v64+localIdx+2])->left = c->PopArg();
			}
			c->PushFrame();
			// func = localIdx
			// retList = localIdx+1
			for (size_t i=0;i<localIdx;++i){
				val = c->argStack.data[v64+i];
				if (GetType(val)!=SlangType::List){
					c->PopFrame();
					val = c->argStack.data[v64+localIdx+1];
					c->PopFrame();
					c->PushArg(val);
					NEXT_INST();
				}
				c->PushArg(((SlangList*)val)->left);
				c->argStack.data[v64+i] = ((SlangList*)val)->right;
			}
			
			val = c->argStack.data[v64+localIdx];
			if (GetType(val)!=SlangType::Lambda){
				if (GetType(val)==SlangType::Symbol&&((SlangObj*)val)->symbol<GLOBAL_SYMBOL_COUNT){
					sym = ((SlangObj*)val)->symbol;
					if (!GoodArity(sym,c->GetArgCount())){
						c->ArityError(
							c->GetArgCount(),
							gGlobalArityArray[sym].min,
							gGlobalArityArray[sym].max
						);
						return;
					}
					if (!CodeBuiltinFuncs[sym](c))
						return;
					c->pc -= SlangOpSizes[SLANG_OP_MAP_STEP];
					NEXT_INST();
				}
				c->TypeError(GetType(val),SlangType::Lambda);
				return;
			}
			
			lam = (SlangLambda*)val;
			boolVal = (val->flags & FLAG_CLOSURE);
			v64 = lam->funcIndex;
			v642 = (uint64_t)c->pc;
			block = &c->codeWriter.lambdaCodes[v64];
			c->lamEnv = lam->env;
			if (!CFHandleArgs(
					c,
					lam->env,
					*block))
				return;
				
			c->Call(v64,c->lamEnv,boolVal);
			c->lamEnv = nullptr;
			c->funcStack.Back().retAddr = (const uint8_t*)v642;
			NEXT_INST();
		LOOP_INST(SLANG_OP_FOREACH_STEP)
			localIdx = *(uint16_t*)(c->pc+OPCODE_SIZE);
			v64 = c->stack.Back().base;

			c->PushFrame();
			// func = localIdx
			for (size_t i=0;i<localIdx;++i){
				val = c->argStack.data[v64+i];
				if (GetType(val)!=SlangType::List){
					c->PopFrame();
					c->PopFrame();
					c->PushArg(nullptr);
					NEXT_INST();
				}
				c->PushArg(((SlangList*)val)->left);
				c->argStack.data[v64+i] = ((SlangList*)val)->right;
			}
			
			val = c->argStack.data[v64+localIdx];
			if (GetType(val)!=SlangType::Lambda){
				if (GetType(val)==SlangType::Symbol&&((SlangObj*)val)->symbol<GLOBAL_SYMBOL_COUNT){
					sym = ((SlangObj*)val)->symbol;
					if (!GoodArity(sym,c->GetArgCount())){
						c->ArityError(
							c->GetArgCount(),
							gGlobalArityArray[sym].min,
							gGlobalArityArray[sym].max
						);
						return;
					}
					if (!CodeBuiltinFuncs[sym](c))
						return;
					c->pc -= SlangOpSizes[SLANG_OP_FOREACH_STEP];
					c->PopArg();
					NEXT_INST();
				}
				c->TypeError(GetType(val),SlangType::Lambda);
				return;
			}
			
			lam = (SlangLambda*)val;
			boolVal = (val->flags & FLAG_CLOSURE);
			v64 = lam->funcIndex;
			v642 = (uint64_t)c->pc;
			block = &c->codeWriter.lambdaCodes[v64];
			c->lamEnv = lam->env;
			if (!CFHandleArgs(
					c,
					lam->env,
					*block))
				return;
				
			c->Call(v64,c->lamEnv,boolVal);
			c->lamEnv = nullptr;
			c->funcStack.Back().retAddr = (const uint8_t*)v642;
			NEXT_INST();
		LOOP_INST(SLANG_OP_FILTER_STEP)
			v64 = c->stack.Back().base;
			
			// first run
			if (c->argStack.size-v64==4){
				val = c->argStack.data[v64];
				// called with empty list
				if (!val){
					c->PopFrame();
					c->PushArg(nullptr);
					NEXT_INST();
				}
				c->PushFrame();
				c->PushArg(((SlangList*)val)->left);
			} else {
				// get bool
				val = c->PopArg();
				if (ConvertToBool(val)){
					val2 = (SlangHeader*)c->alloc.AllocateList();
					if (c->argStack.data[v64+3]){
						((SlangList*)c->argStack.data[v64+3])->right = val2;
						val = c->argStack.data[v64+3];
						c->argStack.data[v64+3] = ((SlangList*)val)->right;
					} else {
						c->argStack.data[v64+2] = val2;
						c->argStack.data[v64+3] = val2;
					}
					((SlangList*)c->argStack.data[v64+3])->left = ((SlangList*)c->argStack.data[v64])->left;
				}
				
				c->argStack.data[v64] = ((SlangList*)c->argStack.data[v64])->right;
				val = c->argStack.data[v64];
				if (GetType(val)!=SlangType::List){
					val = c->argStack.data[v64+2];
					c->PopFrame();
					c->PushArg(val);
					NEXT_INST();
				}
				c->PushFrame();
				c->PushArg(((SlangList*)val)->left);
			}
			
			val = c->argStack.data[v64+1];
			if (GetType(val)!=SlangType::Lambda){
				if (GetType(val)==SlangType::Symbol&&((SlangObj*)val)->symbol<GLOBAL_SYMBOL_COUNT){
					sym = ((SlangObj*)val)->symbol;
					if (!GoodArity(sym,c->GetArgCount())){
						c->ArityError(
							c->GetArgCount(),
							gGlobalArityArray[sym].min,
							gGlobalArityArray[sym].max
						);
						return;
					}
					if (!CodeBuiltinFuncs[sym](c))
						return;
					c->pc -= SlangOpSizes[SLANG_OP_FILTER_STEP];
					NEXT_INST();
				}
				c->TypeError(GetType(val),SlangType::Lambda);
				return;
			}
			
			lam = (SlangLambda*)val;
			boolVal = (val->flags & FLAG_CLOSURE);
			v64 = lam->funcIndex;
			v642 = (uint64_t)c->pc;
			block = &c->codeWriter.lambdaCodes[v64];
			c->lamEnv = lam->env;
			if (!CFHandleArgs(
					c,
					lam->env,
					*block))
				return;
				
			c->Call(v64,c->lamEnv,boolVal);
			c->lamEnv = nullptr;
			c->funcStack.Back().retAddr = (const uint8_t*)v642;
			NEXT_INST();
		LOOP_INST(SLANG_OP_FOLD_STEP)
			v64 = c->stack.Back().base;
			
			// first run
			if (c->argStack.size-v64==3){
				val = c->argStack.data[v64+1];
				// called with empty list
				if (!val){
					val = c->argStack.data[v64];
					c->PopFrame();
					c->PushArg(val);
					NEXT_INST();
				}
				c->PushFrame();
				c->PushArg(c->argStack.data[v64]);
				c->PushArg(((SlangList*)val)->left);
			} else {
				c->argStack.data[v64+1] = ((SlangList*)c->argStack.data[v64+1])->right;
				val = c->argStack.data[v64+1];
				if (GetType(val)!=SlangType::List){
					val = c->PopArg();
					c->PopFrame();
					c->PushArg(val);
					NEXT_INST();
				}
				c->PushFrame();
				--c->stack.Back().base;
				c->PushArg(((SlangList*)val)->left);
			}
			
			val = c->argStack.data[v64+2];
			if (GetType(val)!=SlangType::Lambda){
				if (GetType(val)==SlangType::Symbol&&((SlangObj*)val)->symbol<GLOBAL_SYMBOL_COUNT){
					sym = ((SlangObj*)val)->symbol;
					if (!GoodArity(sym,c->GetArgCount())){
						c->ArityError(
							c->GetArgCount(),
							gGlobalArityArray[sym].min,
							gGlobalArityArray[sym].max
						);
						return;
					}
					if (!CodeBuiltinFuncs[sym](c))
						return;
					c->pc -= SlangOpSizes[SLANG_OP_FOLD_STEP];
					NEXT_INST();
				}
				c->TypeError(GetType(val),SlangType::Lambda);
				return;
			}
			
			lam = (SlangLambda*)val;
			boolVal = (val->flags & FLAG_CLOSURE);
			v64 = lam->funcIndex;
			v642 = (uint64_t)c->pc;
			block = &c->codeWriter.lambdaCodes[v64];
			c->lamEnv = lam->env;
			if (!CFHandleArgs(
					c,
					lam->env,
					*block))
				return;
				
			c->Call(v64,c->lamEnv,boolVal);
			c->lamEnv = nullptr;
			c->funcStack.Back().retAddr = (const uint8_t*)v642;
			NEXT_INST();
		LOOP_INST(SLANG_OP_NOT)
			val = c->PopArg();
			if (ConvertToBool(val)){
				c->PushArg(falseObj);
			} else {
				c->PushArg(trueObj);
			}
			NEXT_INST();
		LOOP_INST(SLANG_OP_INC)
			val = c->PopArg();
			LOOP_TYPE_CHECK_EXACT(val,SlangType::Int);
			c->PushArg((SlangHeader*)c->alloc.MakeInt(((SlangObj*)val)->integer+1));
			NEXT_INST();
		LOOP_INST(SLANG_OP_DEC)
			val = c->PopArg();
			LOOP_TYPE_CHECK_EXACT(val,SlangType::Int);
			c->PushArg((SlangHeader*)c->alloc.MakeInt(((SlangObj*)val)->integer-1));
			NEXT_INST();
		LOOP_INST(SLANG_OP_NEG)
			val = c->PopArg();
			LOOP_TYPE_CHECK_NUMERIC(val);
			
			if (val->type==SlangType::Int){
				c->PushArg((SlangHeader*)c->alloc.MakeInt(-((SlangObj*)val)->integer));
			} else {
				c->PushArg((SlangHeader*)c->alloc.MakeReal(-((SlangObj*)val)->real));
			}
			NEXT_INST();
		LOOP_INST(SLANG_OP_INVERT)
			val = c->PopArg();
			LOOP_TYPE_CHECK_NUMERIC(val);
			
			if (val->type==SlangType::Int){
				if (((SlangObj*)val)->integer==0){
					c->ZeroDivisionError();
					return;
				}
				c->PushArg((SlangHeader*)c->alloc.MakeInt(floordiv(1,((SlangObj*)val)->integer)));
			} else {
				if (((SlangObj*)val)->real==0.0){
					c->ZeroDivisionError();
					return;
				}
				c->PushArg((SlangHeader*)c->alloc.MakeReal(1.0/((SlangObj*)val)->real));
			}
			NEXT_INST();
		LOOP_INST(SLANG_OP_ADD)
			localIdx = *(uint16_t*)(c->pc+OPCODE_SIZE);
			v64 = c->argStack.size-localIdx;
			val = (SlangHeader*)c->alloc.MakeInt(0);
			// int loop
			for (i=0;i<localIdx;++i){
				val2 = c->argStack.data[v64+i];
				LOOP_TYPE_CHECK_NUMERIC(val2);
				
				if (val2->type==SlangType::Real){
					val = (SlangHeader*)c->alloc.MakeReal(
						(double)((SlangObj*)val)->integer+((SlangObj*)val2)->real
					);
					++i;
					break;
				}
					
				((SlangObj*)val)->integer += ((SlangObj*)val2)->integer;
			}
			// real loop
			for (;i<localIdx;++i){
				val2 = c->argStack.data[v64+i];
				LOOP_TYPE_CHECK_NUMERIC(val2);
				
				if (val2->type==SlangType::Real){
					((SlangObj*)val)->real += ((SlangObj*)val2)->real;
				} else {
					((SlangObj*)val)->real += ((SlangObj*)val2)->integer;
				}
			}
			
			c->argStack.size = v64;
			c->PushArg(val);
			NEXT_INST();
		LOOP_INST(SLANG_OP_SUB)
			localIdx = *(uint16_t*)(c->pc+OPCODE_SIZE);
			v64 = c->argStack.size-localIdx;
			val2 = c->argStack.data[v64];
			LOOP_TYPE_CHECK_NUMERIC(val2);
			i = 1;
			if (val2->type==SlangType::Int){
				val = (SlangHeader*)c->alloc.MakeInt(((SlangObj*)val2)->integer);
				// int loop
				for (;i<localIdx;++i){
					val2 = c->argStack.data[v64+i];
					LOOP_TYPE_CHECK_NUMERIC(val2);
					
					if (val2->type==SlangType::Real){
						val = (SlangHeader*)c->alloc.MakeReal(
							(double)((SlangObj*)val)->integer-((SlangObj*)val2)->real
						);
						++i;
						break;
					}
						
					((SlangObj*)val)->integer -= ((SlangObj*)val2)->integer;
				}
			} else {
				val = (SlangHeader*)c->alloc.MakeReal(((SlangObj*)val2)->real);
			}
			
			// real loop
			for (;i<localIdx;++i){
				val2 = c->argStack.data[v64+i];
				LOOP_TYPE_CHECK_NUMERIC(val2);
				
				if (val2->type==SlangType::Real){
					((SlangObj*)val)->real -= ((SlangObj*)val2)->real;
				} else {
					((SlangObj*)val)->real -= ((SlangObj*)val2)->integer;
				}
			}
			
			c->argStack.size = v64;
			c->PushArg(val);
			NEXT_INST();
		LOOP_INST(SLANG_OP_MUL)
			localIdx = *(uint16_t*)(c->pc+OPCODE_SIZE);
			v64 = c->argStack.size-localIdx;
			val = (SlangHeader*)c->alloc.MakeInt(1);
			// int loop
			for (i=0;i<localIdx;++i){
				val2 = c->argStack.data[v64+i];
				LOOP_TYPE_CHECK_NUMERIC(val2);
				
				if (val2->type==SlangType::Real){
					val = (SlangHeader*)c->alloc.MakeReal(
						(double)((SlangObj*)val)->integer*((SlangObj*)val2)->real
					);
					++i;
					break;
				}
					
				((SlangObj*)val)->integer *= ((SlangObj*)val2)->integer;
			}
			// real loop
			for (;i<localIdx;++i){
				val2 = c->argStack.data[v64+i];
				LOOP_TYPE_CHECK_NUMERIC(val2);
				
				if (val2->type==SlangType::Real){
					((SlangObj*)val)->real *= ((SlangObj*)val2)->real;
				} else {
					((SlangObj*)val)->real *= ((SlangObj*)val2)->integer;
				}
			}
			
			c->argStack.size = v64;
			c->PushArg(val);
			NEXT_INST();
		LOOP_INST(SLANG_OP_DIV)
			localIdx = *(uint16_t*)(c->pc+OPCODE_SIZE);
			v64 = c->argStack.size-localIdx;
			val2 = c->argStack.data[v64];
			LOOP_TYPE_CHECK_NUMERIC(val2);
			i = 1;
			if (val2->type==SlangType::Int){
				val = (SlangHeader*)c->alloc.MakeInt(((SlangObj*)val2)->integer);
				// int loop
				for (;i<localIdx;++i){
					val2 = c->argStack.data[v64+i];
					LOOP_TYPE_CHECK_NUMERIC(val2);
					
					if (val2->type==SlangType::Real){
						if (((SlangObj*)val2)->real==0.0){
							c->ZeroDivisionError();
							return;
						}
						val = (SlangHeader*)c->alloc.MakeReal(
							(double)((SlangObj*)val)->integer/((SlangObj*)val2)->real
						);
						++i;
						break;
					}
					if (((SlangObj*)val2)->integer==0){
						c->ZeroDivisionError();
						return;
					}
					((SlangObj*)val)->integer = 
						floordiv(((SlangObj*)val)->integer,((SlangObj*)val2)->integer);
				}
			} else {
				val = (SlangHeader*)c->alloc.MakeReal(((SlangObj*)val2)->real);
			}
			
			// real loop
			for (;i<localIdx;++i){
				val2 = c->argStack.data[v64+i];
				LOOP_TYPE_CHECK_NUMERIC(val2);
				
				if (val2->type==SlangType::Real){
					if (((SlangObj*)val2)->real==0.0){
						c->ZeroDivisionError();
						return;
					}
					((SlangObj*)val)->real /= ((SlangObj*)val2)->real;
				} else {
					if (((SlangObj*)val2)->integer==0){
						c->ZeroDivisionError();
						return;
					}
					((SlangObj*)val)->real /= ((SlangObj*)val2)->integer;
				}
			}
			
			c->argStack.size = v64;
			c->PushArg(val);
			NEXT_INST();
		LOOP_INST(SLANG_OP_EQ)
			val = c->PopArg();
			val2 = c->PopArg();
			if (EqualObjs(val,val2))
				c->PushArg(trueObj);
			else
				c->PushArg(falseObj);
			NEXT_INST();
		LOOP_INST(SLANG_OP_PAIR)
			val = (SlangHeader*)c->alloc.AllocateList();
			((SlangList*)val)->right = c->PopArg();
			((SlangList*)val)->left = c->PopArg();
			c->PushArg(val);
			NEXT_INST();
		LOOP_INST(SLANG_OP_LIST_CONCAT)
		{
			localIdx = *(uint16_t*)(c->pc+OPCODE_SIZE);
			if (localIdx==0){
				c->PushArg(nullptr);
				NEXT_INST();
			}
			
			v64 = c->argStack.size-localIdx;
			list = nullptr;
			for (size_t i=0;i<(uint16_t)(localIdx-1);++i){
				val = c->argStack.data[v64+i];
				
				if (list){
					list->right = val;
				}
				
				if (GetType(val)!=SlangType::List){
					c->TypeError(GetType(val),SlangType::List);
					return;
				}
				
				if (!GetTailOfList((SlangList*)val,(SlangList**)&val2)){
					c->DotError();
					return;
				}
				
				list = (SlangList*)val2;
				if (!c->arena->InCurrSet((uint8_t*)list)){
					c->PushError("SetError","Cannot set right of const data!");
					return;
				}
			}
			
			if (list){
				list->right = c->argStack.data[v64+localIdx-1];
			}
			
			c->argStack.size -= localIdx-1;
			
			NEXT_INST();
		}
		LOOP_INST(SLANG_OP_LEFT)
			val = c->PopArg();
			LOOP_TYPE_CHECK_EXACT(val,SlangType::List);
			c->PushArg(((SlangList*)val)->left);
			NEXT_INST();
		LOOP_INST(SLANG_OP_RIGHT)
			val = c->PopArg();
			LOOP_TYPE_CHECK_EXACT(val,SlangType::List);
			c->PushArg(((SlangList*)val)->right);
			NEXT_INST();
		LOOP_INST(SLANG_OP_SET_LEFT)
			val2 = c->PopArg();
			val = c->PopArg();
			LOOP_TYPE_CHECK_EXACT(val,SlangType::List);
			if (!c->arena->InCurrSet((uint8_t*)val)){
				c->PushError("SetError","Cannot set left of const data!");
				return;
			}
			((SlangList*)val)->left = val2;
			NEXT_INST();
		LOOP_INST(SLANG_OP_SET_RIGHT)
			val2 = c->PopArg();
			val = c->PopArg();
			LOOP_TYPE_CHECK_EXACT(val,SlangType::List);
			if (!c->arena->InCurrSet((uint8_t*)val)){
				c->PushError("SetError","Cannot set right of const data!");
				return;
			}
			((SlangList*)val)->right = val2;
			NEXT_INST();
		LOOP_INST(SLANG_OP_MAKE_VEC)
			val = (SlangHeader*)c->MakeVecFromArgs(c->stack.Back().base,c->argStack.size);
			c->argStack.size = c->stack.Back().base;
			c->PopFrame();
			c->PushArg(val);
			NEXT_INST();
		LOOP_INST(SLANG_OP_VEC_GET)
			val2 = c->PopArg();
			val = c->PopArg();
			assert(GetType(val2)==SlangType::Int);
			i64 = ((SlangObj*)val2)->integer;
			
			LOOP_TYPE_CHECK_EXACT(val,SlangType::Vector);
			
			v642 = ((SlangVec*)val)->storage ? ((SlangVec*)val)->storage->size : 0;
			if (i64<0) i64 += v642;
			if (i64>=(int64_t)v642||i64<0){
				c->IndexError(i64,v642);
				return;
			}
			
			c->PushArg(((SlangVec*)val)->storage->objs[i64]);
			NEXT_INST();
		LOOP_INST(SLANG_OP_VEC_SET)
			val2 = c->PopArg();
			val = c->PopArg();
			assert(GetType(val)==SlangType::Int);
			i64 = ((SlangObj*)val)->integer;
			
			val = c->PopArg();
			LOOP_TYPE_CHECK_EXACT(val,SlangType::Vector);
			if (!c->arena->InCurrSet((uint8_t*)val)){
				c->PushError("SetError","Cannot set const vector data!");
				return;
			}
			
			// size
			v642 = ((SlangVec*)val)->storage ? ((SlangVec*)val)->storage->size : 0;
			if (i64<0) i64 += v642;
			if (i64>=(int64_t)v642||i64<0){
				c->IndexError(i64,v642);
				return;
			}
			
			((SlangVec*)val)->storage->objs[i64] = val2;
			NEXT_INST();
		LOOP_INST(SLANG_OP_EXPORT)
			sym = *(SymbolName*)(c->pc+OPCODE_SIZE);
			
			v64 = c->funcStack.Back().funcIndex;
			v642 = c->codeWriter.lambdaCodes[v64].moduleIndex;
			env = c->modules.data[v642].exportEnv;
			if (!env){
				NEXT_INST();
			}
			
			newEnv = c->GetCurrEnv();
			if (!GetRecursiveSymbol(newEnv,sym,&val)){
				c->UndefinedError(sym);
				return;
			}
			
			if (env->GetSymbol(sym,&val2)){
				c->ExportError(sym);
				return;
			}
			c->DefEnvSymbol(env,sym,val);
			NEXT_INST();
		LOOP_INST(SLANG_OP_IMPORT)
			val = *(SlangHeader**)(c->pc+OPCODE_SIZE);
			if (!CFHandleImport(c,(SlangList*)val))
				return;
			NEXT_INST();
	
	LOOP_POST()
}

