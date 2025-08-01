gb_internal LLVMValueRef lb_call_intrinsic(lbProcedure *p, const char *name, LLVMValueRef* args, unsigned arg_count, LLVMTypeRef* types, unsigned type_count) {
	unsigned id = LLVMLookupIntrinsicID(name, gb_strlen(name));
	GB_ASSERT_MSG(id != 0, "Unable to find %s", name);
	LLVMValueRef ip = LLVMGetIntrinsicDeclaration(p->module->mod, id, types, type_count);
	LLVMTypeRef call_type = LLVMIntrinsicGetType(p->module->ctx, id, types, type_count);
	return LLVMBuildCall2(p->builder, call_type, ip, args, arg_count, "");
}

gb_internal void lb_mem_copy_overlapping(lbProcedure *p, lbValue dst, lbValue src, lbValue len, bool is_volatile) {
	dst = lb_emit_conv(p, dst, t_rawptr);
	src = lb_emit_conv(p, src, t_rawptr);
	len = lb_emit_conv(p, len, t_int);
	
	char const *name = "llvm.memmove";
	if (LLVMIsConstant(len.value)) {
		i64 const_len = cast(i64)LLVMConstIntGetSExtValue(len.value);
		if (const_len <= 4*build_context.int_size) {
			name = "llvm.memmove.inline";
		}
	}
	LLVMTypeRef types[3] = {
		lb_type(p->module, t_rawptr),
		lb_type(p->module, t_rawptr),
		lb_type(p->module, t_int)
	};
	LLVMValueRef args[4] = {
		dst.value,
		src.value,
		len.value,
		LLVMConstInt(LLVMInt1TypeInContext(p->module->ctx), 0, is_volatile)
	};

	lb_call_intrinsic(p, name, args, gb_count_of(args), types, gb_count_of(types));
}



gb_internal void lb_mem_copy_non_overlapping(lbProcedure *p, lbValue dst, lbValue src, lbValue len, bool is_volatile) {
	dst = lb_emit_conv(p, dst, t_rawptr);
	src = lb_emit_conv(p, src, t_rawptr);
	len = lb_emit_conv(p, len, t_int);
	
	char const *name = "llvm.memcpy";
	if (LLVMIsConstant(len.value)) {
		i64 const_len = cast(i64)LLVMConstIntGetSExtValue(len.value);
		if (const_len <= 4*build_context.int_size) {
			name = "llvm.memcpy.inline";
		}
	}

	LLVMTypeRef types[3] = {
		lb_type(p->module, t_rawptr),
		lb_type(p->module, t_rawptr),
		lb_type(p->module, t_int)
	};

	LLVMValueRef args[4] = {
			dst.value,
			src.value,
			len.value,
			LLVMConstInt(LLVMInt1TypeInContext(p->module->ctx), 0, is_volatile) };

	lb_call_intrinsic(p, name, args, gb_count_of(args), types, gb_count_of(types));
}


gb_internal lbProcedure *lb_create_procedure(lbModule *m, Entity *entity, bool ignore_body) {
	GB_ASSERT(entity != nullptr);
	GB_ASSERT(entity->kind == Entity_Procedure);
	// Skip codegen for unspecialized polymorphic procedures
	if (is_type_polymorphic(entity->type) && !entity->Procedure.is_foreign) {
		Type *bt = base_type(entity->type);
		if (bt->kind == Type_Proc && bt->Proc.is_polymorphic && !bt->Proc.is_poly_specialized) {
			// Do not generate code for unspecialized polymorphic procedures
			return nullptr;
		}
	}
	if (!entity->Procedure.is_foreign) {
		if ((entity->flags & EntityFlag_ProcBodyChecked) == 0) {
			GB_PANIC("%.*s :: %s (was parapoly: %d %d)", LIT(entity->token.string), type_to_string(entity->type), is_type_polymorphic(entity->type, true), is_type_polymorphic(entity->type, false));
		}
	}

	String link_name = {};

	if (ignore_body) {
		lbModule *other_module = lb_module_of_entity(m->gen, entity);
		link_name = lb_get_entity_name(other_module, entity);
	} else {
		link_name = lb_get_entity_name(m, entity);
	}

	{
		StringHashKey key = string_hash_string(link_name);
		lbValue *found = string_map_get(&m->members, key);
		if (found) {
			lb_add_entity(m, entity, *found);
			return string_map_must_get(&m->procedures, key);
		}
	}


	lbProcedure *p = gb_alloc_item(permanent_allocator(), lbProcedure);

	p->module = m;
	entity->code_gen_module = m;
	entity->code_gen_procedure = p;
	p->entity = entity;
	p->name = link_name;

	DeclInfo *decl = entity->decl_info;

	ast_node(pl, ProcLit, decl->proc_lit);
	Type *pt = base_type(entity->type);
	GB_ASSERT(pt->kind == Type_Proc);

	p->type           = entity->type;
	p->type_expr      = decl->type_expr;
	p->body           = pl->body;
	p->inlining       = pl->inlining;
	p->is_foreign     = entity->Procedure.is_foreign;
	p->is_export      = entity->Procedure.is_export;
	p->is_entry_point = false;

	gbAllocator a = heap_allocator();
	p->children.allocator          = a;
	p->defer_stmts.allocator       = a;
	p->blocks.allocator            = a;
	p->branch_blocks.allocator     = a;
	p->context_stack.allocator     = a;
	p->scope_stack.allocator       = a;
	p->asan_stack_locals.allocator = a;
	// map_init(&p->selector_values,  0);
	// map_init(&p->selector_addr,    0);
	// map_init(&p->tuple_fix_map,    0);

	if (p->entity != nullptr && p->entity->Procedure.uses_branch_location) {
		p->uses_branch_location = true;
	}

	if (p->is_foreign) {
		lb_add_foreign_library_path(p->module, entity->Procedure.foreign_library);
	}

	LLVMTypeRef func_type = lb_get_procedure_raw_type(m, p->type);

	{
		TEMPORARY_ALLOCATOR_GUARD();
		char *c_link_name = alloc_cstring(temporary_allocator(), p->name);
		p->value = LLVMAddFunction(m->mod, c_link_name, func_type);
	}

	lb_ensure_abi_function_type(m, p);
	lb_add_function_type_attributes(p->value, p->abi_function_type, p->abi_function_type->calling_convention);

	if (pt->Proc.diverging) {
		lb_add_attribute_to_proc(m, p->value, "noreturn");
	}

	if (pt->Proc.calling_convention == ProcCC_Naked) {
		lb_add_attribute_to_proc(m, p->value, "naked");
	}

	if (!entity->Procedure.is_foreign && build_context.disable_red_zone) {
		lb_add_attribute_to_proc(m, p->value, "noredzone");
	}

	switch (p->inlining) {
	case ProcInlining_inline:
		lb_add_attribute_to_proc(m, p->value, "alwaysinline");
		break;
	case ProcInlining_no_inline:
		lb_add_attribute_to_proc(m, p->value, "noinline");
		break;
	default:
		if (build_context.internal_no_inline) {
			lb_add_attribute_to_proc(m, p->value, "noinline");
			break;
		}
	}

	switch (entity->Procedure.optimization_mode) {
	case ProcedureOptimizationMode_None:
		lb_add_attribute_to_proc(m, p->value, "optnone");
		lb_add_attribute_to_proc(m, p->value, "noinline");
		break;
	case ProcedureOptimizationMode_FavorSize:
		lb_add_attribute_to_proc(m, p->value, "optsize");
		break;
	}

	if (pt->Proc.enable_target_feature.len != 0) {
		gbString feature_str = gb_string_make(temporary_allocator(), "");

		String_Iterator it = {pt->Proc.enable_target_feature, 0};
		bool first = true;
		for (;;) {
			String str = string_split_iterator(&it, ',');
			if (str == "") break;
			if (!first) {
				feature_str = gb_string_appendc(feature_str, ",");
			}
			first = false;

			feature_str = gb_string_appendc(feature_str, "+");
			feature_str = gb_string_append_length(feature_str, str.text, str.len);
		}

		lb_add_attribute_to_proc_with_string(m, p->value, make_string_c("target-features"), make_string_c(feature_str));
	}

	if (entity->flags & EntityFlag_Cold) {
		lb_add_attribute_to_proc(m, p->value, "cold");
	}

	if (p->is_export) {
		LLVMSetLinkage(p->value, LLVMDLLExportLinkage);
		LLVMSetDLLStorageClass(p->value, LLVMDLLExportStorageClass);
		LLVMSetVisibility(p->value, LLVMDefaultVisibility);

		lb_set_wasm_export_attributes(p->value, p->name);
	} else if (!p->is_foreign) {
		if (USE_SEPARATE_MODULES) {
			LLVMSetLinkage(p->value, LLVMExternalLinkage);
		} else {
			LLVMSetLinkage(p->value, LLVMInternalLinkage);

			// NOTE(bill): if a procedure is defined in package runtime and uses a custom link name,
			// then it is very likely it is required by LLVM and thus cannot have internal linkage
			if (entity->pkg != nullptr && entity->pkg->kind == Package_Runtime && p->body != nullptr) {
				GB_ASSERT(entity->kind == Entity_Procedure);
				String link_name = entity->Procedure.link_name;
				if (entity->flags & EntityFlag_CustomLinkName && 
					link_name != "") {
					if (string_starts_with(link_name, str_lit("__"))) {
						LLVMSetLinkage(p->value, LLVMExternalLinkage);
					} else {
						LLVMSetLinkage(p->value, LLVMInternalLinkage);
					}
				}
			}
		}
	}
	lb_set_linkage_from_entity_flags(p->module, p->value, entity->flags);


	if (p->is_foreign) {
		lb_set_wasm_procedure_import_attributes(p->value, entity, p->name);
	}


	// NOTE(bill): offset==0 is the return value
	isize offset = 1;
	if (pt->Proc.return_by_pointer) {
		offset = 2;
	}

	isize parameter_index = 0;
	if (pt->Proc.param_count) {
		TypeTuple *params = &pt->Proc.params->Tuple;
		for (isize i = 0; i < pt->Proc.param_count; i++) {
			Entity *e = params->variables[i];
			if (e->kind != Entity_Variable) {
				continue;
			}

			if (i+1 == params->variables.count && pt->Proc.c_vararg) {
				continue;
			}

			if (e->flags&EntityFlag_NoAlias) {
				lb_add_proc_attribute_at_index(p, offset+parameter_index, "noalias");
			}
			if (e->flags&EntityFlag_NoCapture) {
				if (is_type_internally_pointer_like(e->type)) {
					lb_add_proc_attribute_at_index(p, offset+parameter_index, "nocapture");
				}
			}
			parameter_index += 1;
		}
	}

	if (ignore_body) {
		p->body = nullptr;
		LLVMSetLinkage(p->value, LLVMExternalLinkage);
	}


	if (m->debug_builder) { // Debug Information
		Type *bt = base_type(p->type);

		unsigned line = cast(unsigned)entity->token.pos.line;

		LLVMMetadataRef scope = nullptr;
		LLVMMetadataRef file = nullptr;
		LLVMMetadataRef type = nullptr;
		scope = p->module->debug_compile_unit;
		type = lb_debug_type_internal_proc(m, bt);

		Ast *ident = entity->identifier.load();
		if (entity->file != nullptr) {
			file = lb_get_llvm_metadata(m, entity->file);
			scope = file;
		} else if (ident != nullptr && ident->file_id != 0) {
			file = lb_get_llvm_metadata(m, ident->file());
			scope = file;
		} else if (entity->scope != nullptr) {
			file = lb_get_llvm_metadata(m, entity->scope->file);
			scope = file;
		}
		GB_ASSERT_MSG(file != nullptr, "%.*s", LIT(entity->token.string));

		// LLVMBool is_local_to_unit = !entity->Procedure.is_export;
		LLVMBool is_local_to_unit = false;
		LLVMBool is_definition = p->body != nullptr;
		unsigned scope_line = line;
		u32 flags = LLVMDIFlagStaticMember;
		LLVMBool is_optimized = false;
		if (bt->Proc.diverging) {
			flags |= LLVMDIFlagNoReturn;
		}
		if (p->body == nullptr) {
			flags |= LLVMDIFlagPrototyped;
			is_optimized = false;
		}

		if (p->body != nullptr) {
			// String debug_name = entity->token.string.text;
			String debug_name = p->name;

			p->debug_info = LLVMDIBuilderCreateFunction(m->debug_builder, scope,
				cast(char const *)debug_name.text, debug_name.len,
				cast(char const *)p->name.text, p->name.len,
				file, line, type,
				is_local_to_unit, is_definition,
				scope_line, cast(LLVMDIFlags)flags, is_optimized
			);
			GB_ASSERT(p->debug_info != nullptr);
			LLVMSetSubprogram(p->value, p->debug_info);
			lb_set_llvm_metadata(m, p, p->debug_info);
		}
	}

	if (p->body && entity->pkg && ((entity->pkg->kind == Package_Normal) || (entity->pkg->kind == Package_Init))) {
		if (build_context.sanitizer_flags & SanitizerFlag_Address && !entity->Procedure.no_sanitize_address) {
			lb_add_attribute_to_proc(m, p->value, "sanitize_address");
		}
		if (build_context.sanitizer_flags & SanitizerFlag_Memory && !entity->Procedure.no_sanitize_memory) {
			lb_add_attribute_to_proc(m, p->value, "sanitize_memory");
		}
		if (build_context.sanitizer_flags & SanitizerFlag_Thread) {
			lb_add_attribute_to_proc(m, p->value, "sanitize_thread");
		}
	}

	if (p->body && entity->Procedure.has_instrumentation) {
		Entity *instrumentation_enter = m->info->instrumentation_enter_entity;
		Entity *instrumentation_exit  = m->info->instrumentation_exit_entity;
		if (instrumentation_enter && instrumentation_exit) {
			String enter = lb_get_entity_name(m, instrumentation_enter);
			String exit  = lb_get_entity_name(m, instrumentation_exit);

			lb_add_attribute_to_proc_with_string(m, p->value, make_string_c("instrument-function-entry"), enter);
			lb_add_attribute_to_proc_with_string(m, p->value, make_string_c("instrument-function-exit"),  exit);
		}
	}

	lbValue proc_value = {p->value, p->type};
	lb_add_entity(m, entity,  proc_value);
	lb_add_member(m, p->name, proc_value);
	lb_add_procedure_value(m, p);

	return p;
}

gb_internal lbProcedure *lb_create_dummy_procedure(lbModule *m, String link_name, Type *type) {
	{
		lbValue *found = string_map_get(&m->members, link_name);
		GB_ASSERT_MSG(found == nullptr, "failed to create dummy procedure for: %.*s", LIT(link_name));
	}

	lbProcedure *p = gb_alloc_item(permanent_allocator(), lbProcedure);

	p->module = m;
	p->name = link_name;

	p->type           = type;
	p->type_expr      = nullptr;
	p->body           = nullptr;
	p->tags           = 0;
	p->inlining       = ProcInlining_none;
	p->is_foreign     = false;
	p->is_export      = false;
	p->is_entry_point = false;

	gbAllocator a = permanent_allocator();
	p->children.allocator          = a;
	p->defer_stmts.allocator       = a;
	p->blocks.allocator            = a;
	p->branch_blocks.allocator     = a;
	p->context_stack.allocator     = a;
	p->asan_stack_locals.allocator = a;
	map_init(&p->tuple_fix_map, 0);


	char *c_link_name = alloc_cstring(permanent_allocator(), p->name);
	LLVMTypeRef func_type = lb_get_procedure_raw_type(m, p->type);

	p->value = LLVMAddFunction(m->mod, c_link_name, func_type);

	Type *pt = p->type;
	lbCallingConventionKind cc_kind = lbCallingConvention_C;
	if (!is_arch_wasm()) {
		cc_kind = lb_calling_convention_map[pt->Proc.calling_convention];
	}
	LLVMSetFunctionCallConv(p->value, cc_kind);
	lbValue proc_value = {p->value, p->type};
	lb_add_member(m, p->name, proc_value);
	lb_add_procedure_value(m, p);


	// NOTE(bill): offset==0 is the return value
	isize offset = 1;
	if (pt->Proc.return_by_pointer) {
		lb_add_proc_attribute_at_index(p, 1, "sret");
		lb_add_proc_attribute_at_index(p, 1, "noalias");
		offset = 2;
	}

	isize parameter_index = 0;
	if (pt->Proc.calling_convention == ProcCC_Odin) {
		lb_add_proc_attribute_at_index(p, offset+parameter_index, "noalias");
		lb_add_proc_attribute_at_index(p, offset+parameter_index, "nonnull");
		lb_add_proc_attribute_at_index(p, offset+parameter_index, "nocapture");
	}
	return p;
}


// gb_internal lbValue lb_value_param(lbProcedure *p, Entity *e, Type *abi_type, i32 index, lbParamPasskind *kind_) {
// 	lbParamPasskind kind = lbParamPass_Value;

// 	if (e != nullptr && !are_types_identical(abi_type, e->type)) {
// 		if (is_type_pointer(abi_type)) {
// 			GB_ASSERT(e->kind == Entity_Variable);
// 			Type *av = core_type(type_deref(abi_type));
// 			if (are_types_identical(av, core_type(e->type))) {
// 				kind = lbParamPass_Pointer;
// 				if (e->flags&EntityFlag_Value) {
// 					kind = lbParamPass_ConstRef;
// 				}
// 			} else {
// 				kind = lbParamPass_BitCast;
// 			}
// 		} else if (is_type_integer(abi_type)) {
// 			kind = lbParamPass_Integer;
// 		} else if (abi_type == t_llvm_bool) {
// 			kind = lbParamPass_Value;
// 		} else if (is_type_boolean(abi_type)) {
// 			kind = lbParamPass_Integer;
// 		} else if (is_type_simd_vector(abi_type)) {
// 			kind = lbParamPass_BitCast;
// 		} else if (is_type_float(abi_type)) {
// 			kind = lbParamPass_BitCast;
// 		} else if (is_type_tuple(abi_type)) {
// 			kind = lbParamPass_Tuple;
// 		} else if (is_type_proc(abi_type)) {
// 			kind = lbParamPass_Value;
// 		} else {
// 			GB_PANIC("Invalid abi type pass kind %s", type_to_string(abi_type));
// 		}
// 	}

// 	if (kind_) *kind_ = kind;
// 	lbValue res = {};
// 	res.value = LLVMGetParam(p->value, cast(unsigned)index);
// 	res.type = abi_type;
// 	return res;
// }



gb_internal void lb_start_block(lbProcedure *p, lbBlock *b) {
	GB_ASSERT(b != nullptr);
	if (!b->appended) {
		b->appended = true;
		LLVMAppendExistingBasicBlock(p->value, b->block);
	}
	LLVMPositionBuilderAtEnd(p->builder, b->block);
	p->curr_block = b;
}

gb_internal void lb_set_debug_position_to_procedure_begin(lbProcedure *p) {
	if (p->debug_info == nullptr) {
		return;
	}
	TokenPos pos = {};
	if (p->body != nullptr) {
		pos = ast_token(p->body).pos;
	} else if (p->type_expr != nullptr) {
		pos = ast_token(p->type_expr).pos;
	} else if (p->entity != nullptr) {
		pos = p->entity->token.pos;
	}
	if (pos.file_id != 0) {
		LLVMSetCurrentDebugLocation2(p->builder, lb_debug_location_from_token_pos(p, pos));
	}
}

gb_internal void lb_set_debug_position_to_procedure_end(lbProcedure *p) {
	if (p->debug_info == nullptr) {
		return;
	}
	TokenPos pos = {};
	if (p->body != nullptr) {
		pos = ast_end_token(p->body).pos;
	} else if (p->type_expr != nullptr) {
		pos = ast_end_token(p->type_expr).pos;
	} else if (p->entity != nullptr) {
		pos = p->entity->token.pos;
	}
	if (pos.file_id != 0) {
		LLVMSetCurrentDebugLocation2(p->builder, lb_debug_location_from_token_pos(p, pos));
	}
}

gb_internal void lb_begin_procedure_body(lbProcedure *p) {
	DeclInfo *decl = decl_info_of_entity(p->entity);
	if (decl != nullptr) {
		for_array(i, decl->labels) {
			BlockLabel bl = decl->labels[i];
			lbBranchBlocks bb = {bl.label, nullptr, nullptr};
			array_add(&p->branch_blocks, bb);
		}
	}

	p->builder = LLVMCreateBuilderInContext(p->module->ctx);

	p->decl_block  = lb_create_block(p, "decls", true);
	p->entry_block = lb_create_block(p, "entry", true);
	lb_start_block(p, p->entry_block);

	map_init(&p->direct_parameters);
	p->variadic_reuses.allocator = heap_allocator();

	GB_ASSERT(p->type != nullptr);

	lb_ensure_abi_function_type(p->module, p);
	if (p->type->Proc.calling_convention == ProcCC_Odin) {
		lb_push_context_onto_stack_from_implicit_parameter(p);
	}
	{
		lbFunctionType *ft = p->abi_function_type;

		unsigned param_offset = 0;

		lbValue return_ptr_value = {};
		if (ft->ret.kind == lbArg_Indirect) {
			// NOTE(bill): this must be parameter 0

			String name = str_lit("agg.result");
			if (ft->multiple_return_original_type &&
			    p->type->Proc.has_named_results) {
				auto const &variables = p->type->Proc.results->Tuple.variables;
				Entity *e = variables[variables.count-1];
				if (!is_blank_ident(e->token)) {
					name = e->token.string;
				}
			}

			Type *return_ptr_type = reduce_tuple_to_single_type(p->type->Proc.results);
			bool split_returns = ft->multiple_return_original_type != nullptr;
			if (split_returns) {
				GB_ASSERT(is_type_tuple(return_ptr_type));
				auto const &variables = return_ptr_type->Tuple.variables;
				return_ptr_type = variables[variables.count-1]->type;
			}
			Type *ptr_type = alloc_type_pointer(return_ptr_type);
			Entity *e = alloc_entity_param(nullptr, make_token_ident(name), ptr_type, false, false);
			e->flags |= EntityFlag_NoAlias;

			return_ptr_value.value = LLVMGetParam(p->value, 0);
			LLVMSetValueName2(return_ptr_value.value, cast(char const *)name.text, name.len);
			return_ptr_value.type = ptr_type;
			p->return_ptr = lb_addr(return_ptr_value);

			lb_add_entity(p->module, e, return_ptr_value);

			param_offset += 1;
		}

		if (p->type->Proc.params != nullptr) {
			TypeTuple *params = &p->type->Proc.params->Tuple;

			unsigned raw_input_parameters_count = LLVMCountParams(p->value);
			p->raw_input_parameters = array_make<LLVMValueRef>(permanent_allocator(), raw_input_parameters_count);
			LLVMGetParams(p->value, p->raw_input_parameters.data);

			bool is_odin_cc = is_calling_convention_odin(ft->calling_convention);

			unsigned param_index = 0;
			for_array(i, params->variables) {
				Entity *e = params->variables[i];
				if (e->kind != Entity_Variable) {
					continue;
				}

				lbArgType *arg_type = &ft->args[param_index];
				defer (param_index += 1);

				if (arg_type->kind == lbArg_Ignore) {
					// Even though it is an ignored argument, it might still be referenced in the
					// body.
					lbValue dummy = lb_add_local_generated(p, e->type, false).addr;
					lb_add_entity(p->module, e, dummy);
				} else if (arg_type->kind == lbArg_Direct) {
					if (e->token.string.len != 0 && !is_blank_ident(e->token.string)) {
						LLVMTypeRef param_type = lb_type(p->module, e->type);
						LLVMValueRef original_value = LLVMGetParam(p->value, param_offset+param_index);
						LLVMValueRef value = OdinLLVMBuildTransmute(p, original_value, param_type);

						lbValue param = {};
						param.value = value;
						param.type = e->type;

						map_set(&p->direct_parameters, e, param);

						lbValue ptr = lb_address_from_load_or_generate_local(p, param);
						GB_ASSERT(LLVMIsAAllocaInst(ptr.value));
						lb_add_entity(p->module, e, ptr);
						lb_add_debug_param_variable(p, ptr.value, e->type, e->token, param_index+1, p->curr_block);
					}
				} else if (arg_type->kind == lbArg_Indirect) {
					if (e->token.string.len != 0 && !is_blank_ident(e->token.string)) {
						i64 sz = type_size_of(e->type);
						bool do_callee_copy = false;

						if (is_odin_cc) {
							do_callee_copy = sz <= 16;
							if (build_context.internal_by_value) {
								do_callee_copy = true;
							}
						}

						lbValue ptr = {};
						ptr.value = LLVMGetParam(p->value, param_offset+param_index);
						ptr.type = alloc_type_pointer(e->type);

						if (do_callee_copy) {
							lbValue new_ptr = lb_add_local_generated(p, e->type, false).addr;
							lb_mem_copy_non_overlapping(p, new_ptr, ptr, lb_const_int(p->module, t_uint, sz));
							ptr = new_ptr;
						}

						lb_add_entity(p->module, e, ptr);
						lb_add_debug_param_variable(p, ptr.value, e->type, e->token, param_index+1, p->decl_block);
					}
				}
			}
		}

		if (p->type->Proc.has_named_results) {
			GB_ASSERT(p->type->Proc.result_count > 0);
			TypeTuple *results = &p->type->Proc.results->Tuple;

			for_array(i, results->variables) {
				Entity *e = results->variables[i];
				GB_ASSERT(e->kind == Entity_Variable);

				if (e->token.string != "") {
					GB_ASSERT(!is_blank_ident(e->token));

					lbAddr res = {};
					if (p->entity && p->entity->decl_info &&
					    p->entity->decl_info->defer_use_checked &&
					    p->entity->decl_info->defer_used == 0) {

						// NOTE(bill): this is a bodge to get around the issue of the problem BELOW
						// We check to see if we ever use a defer statement ever within a procedure and if it
						// if it never happens, see if you can possibly do take the return value pointer
						//
						// NOTE(bill): this could be buggy in that I have missed a case where `defer` was used
						//
						// TODO(bill): This could be optimized to check to see where a `defer` only uses
						// the variable in question

						bool has_return_ptr = p->return_ptr.addr.value != nullptr;
						lbValue ptr = {};

						if (ft->multiple_return_original_type != nullptr) {
							isize the_offset = -1;
							if (i+1 < results->variables.count) {
								the_offset = cast(isize)param_offset + ft->original_arg_count + i;
							} else if (has_return_ptr) {
								GB_ASSERT(i+1 == results->variables.count);
								the_offset = 0;
							}
							if (the_offset >= 0) {
								lbValue ptr = {};
								ptr.value = LLVMGetParam(p->value, cast(unsigned)the_offset);
								ptr.type = alloc_type_pointer(e->type);


							}
						} else if (has_return_ptr) {
							lbValue ptr = p->return_ptr.addr;

							if (results->variables.count > 1) {
								ptr = lb_emit_tuple_ep(p, ptr, cast(i32)i);
							}
							GB_ASSERT(is_type_pointer(ptr.type));
							GB_ASSERT(are_types_identical(type_deref(ptr.type), e->type));
						}

						if (ptr.value != nullptr) {
							lb_add_entity(p->module, e, ptr);
							lb_add_debug_local_variable(p, ptr.value, e->type, e->token);
							// NOTE(bill): no need to zero on the callee side as it is zeroed on the caller side

							res = lb_addr(ptr);
						}
					}

					if (res.addr.type == nullptr) {
						// NOTE(bill): Don't even bother trying to optimize this with the return ptr value
						// This will violate the defer rules if you do:
						//         foo :: proc() -> (x, y: T) {
						//                 defer x = ... // defer is executed after the `defer`
						//                 return // the values returned should be zeroed
						//         }
						// NOTE(bill): REALLY, don't even bother.
						//
						// IMPORTANT NOTE(bill): REALLY, don't even bother!!!!!!
						res = lb_add_local(p, e->type, e);
					}

					if (e->Variable.param_value.kind != ParameterValue_Invalid) {
						GB_ASSERT(e->Variable.param_value.kind != ParameterValue_Location);
						GB_ASSERT(e->Variable.param_value.kind != ParameterValue_Expression);
						lbValue c = lb_handle_param_value(p, e->type, e->Variable.param_value, nullptr, nullptr);
						lb_addr_store(p, res, c);
					}

				}
			}

		}
	}

	lb_set_debug_position_to_procedure_begin(p);
	if (p->debug_info != nullptr) {
		if (p->context_stack.count != 0) {
			lbBlock *prev_block = p->curr_block;
			p->curr_block = p->decl_block;
			lb_add_debug_context_variable(p, lb_find_or_generate_context_ptr(p));
			p->curr_block = prev_block;
		}
	}
}

gb_internal void lb_end_procedure_body(lbProcedure *p) {
	lb_set_debug_position_to_procedure_begin(p);

	LLVMPositionBuilderAtEnd(p->builder, p->decl_block->block);
	LLVMBuildBr(p->builder, p->entry_block->block);
	LLVMPositionBuilderAtEnd(p->builder, p->curr_block->block);

	LLVMValueRef instr = nullptr;

	// Make sure there is a "ret void" at the end of a procedure with no return type
	if (p->type->Proc.result_count == 0) {
		instr = LLVMGetLastInstruction(p->curr_block->block);
		if (!lb_is_instr_terminating(instr)) {
			lb_emit_defer_stmts(p, lbDeferExit_Return, nullptr, p->body);
			lb_set_debug_position_to_procedure_end(p);
			LLVMBuildRetVoid(p->builder);
		}
	}

	LLVMBasicBlockRef first_block = LLVMGetFirstBasicBlock(p->value);
	LLVMBasicBlockRef block = nullptr;

	// Make sure every block terminates, and if not, make it unreachable
	for (block = first_block; block != nullptr; block = LLVMGetNextBasicBlock(block)) {
		instr = LLVMGetLastInstruction(block);
		if (instr == nullptr || !lb_is_instr_terminating(instr)) {
			LLVMPositionBuilderAtEnd(p->builder, block);
			LLVMBuildUnreachable(p->builder);
		}
	}

	p->curr_block = nullptr;
	p->state_flags = 0;

	LLVMDisposeBuilder(p->builder);
}

gb_internal void lb_build_nested_proc(lbProcedure *p, AstProcLit *pd, Entity *e) {
	GB_ASSERT(pd->body != nullptr);
	lbModule *m = p->module;
	auto *min_dep_set = &m->info->minimum_dependency_set;

	if (ptr_set_exists(min_dep_set, e) == false) {
		// NOTE(bill): Nothing depends upon it so doesn't need to be built
		return;
	}

	// NOTE(bill): Generate a new name
	// parent.name-guid
	String original_name = e->token.string;
	String pd_name = original_name;
	if (e->Procedure.link_name.len > 0) {
		pd_name = e->Procedure.link_name;
	}


	isize name_len = p->name.len + 1 + pd_name.len + 1 + 10 + 1;
	char *name_text = gb_alloc_array(permanent_allocator(), char, name_len);

	i32 guid = cast(i32)p->children.count;
	name_len = gb_snprintf(name_text, name_len, "%.*s" ABI_PKG_NAME_SEPARATOR "%.*s-%d", LIT(p->name), LIT(pd_name), guid);
	String name = make_string(cast(u8 *)name_text, name_len-1);

	e->Procedure.link_name = name;

	lbProcedure *nested_proc = lb_create_procedure(p->module, e);
	if (nested_proc == nullptr) {
		// This is an unspecialized polymorphic procedure, skip codegen
		return;
	}
	e->code_gen_procedure = nested_proc;

	lbValue value = {};
	value.value = nested_proc->value;
	value.type = nested_proc->type;

	lb_add_entity(m, e, value);
	array_add(&p->children, nested_proc);
	array_add(&m->procedures_to_generate, nested_proc);
}



gb_internal Array<lbValue> lb_value_to_array(lbProcedure *p, gbAllocator const &allocator, lbValue value) {
	Array<lbValue> array = {};
	Type *t = base_type(value.type);
	if (t == nullptr) {
		// Do nothing
	} else if (is_type_tuple(t)) {
		array = array_make<lbValue>(allocator, 0, t->Tuple.variables.count);
		lb_append_tuple_values(p, &array, value);
	} else {
		array = array_make<lbValue>(allocator, 1);
		array[0] = value;
	}
	return array;
}



gb_internal lbValue lb_emit_call_internal(lbProcedure *p, lbValue value, lbValue return_ptr, Array<lbValue> const &processed_args, Type *abi_rt, lbAddr context_ptr, ProcInlining inlining) {
	GB_ASSERT(p->module->ctx == LLVMGetTypeContext(LLVMTypeOf(value.value)));

	unsigned arg_count = cast(unsigned)processed_args.count;
	if (return_ptr.value != nullptr) {
		arg_count += 1;
	}
	if (context_ptr.addr.value != nullptr) {
		arg_count += 1;
	}

	LLVMValueRef *args = gb_alloc_array(permanent_allocator(), LLVMValueRef, arg_count);
	isize arg_index = 0;
	if (return_ptr.value != nullptr) {
		args[arg_index++] = return_ptr.value;
	}

	for_array(i, processed_args) {
		lbValue arg = processed_args[i];
		if (is_type_proc(arg.type)) {
			arg.value = LLVMBuildPointerCast(p->builder, arg.value, lb_type(p->module, arg.type), "");
		}
		args[arg_index++] = arg.value;
	}

	if (context_ptr.addr.value != nullptr) {
		LLVMValueRef cp = context_ptr.addr.value;
		cp = LLVMBuildPointerCast(p->builder, cp, lb_type(p->module, t_rawptr), "");
		args[arg_index++] = cp;
	}

	GB_ASSERT(arg_index == arg_count);

	LLVMBasicBlockRef curr_block = LLVMGetInsertBlock(p->builder);
	GB_ASSERT(curr_block != p->decl_block->block);

	{
		Type *proc_type = base_type(value.type);
		GB_ASSERT(proc_type->kind == Type_Proc);

		LLVMTypeRef fnp = lb_type_internal_for_procedures_raw(p->module, proc_type);
		LLVMTypeRef ftp = LLVMPointerType(fnp, 0);
		LLVMValueRef fn = value.value;
		if (!lb_is_type_kind(LLVMTypeOf(value.value), LLVMFunctionTypeKind)) {
			fn = LLVMBuildPointerCast(p->builder, fn, ftp, "");
		}
		GB_ASSERT_MSG(lb_is_type_kind(fnp, LLVMFunctionTypeKind), "%s", LLVMPrintTypeToString(fnp));

		lbFunctionType *ft = map_must_get(&p->module->function_type_map, base_type(value.type));

		{
			unsigned param_count = LLVMCountParamTypes(fnp);
			GB_ASSERT(arg_count >= param_count);

			LLVMTypeRef *param_types = gb_alloc_array(temporary_allocator(), LLVMTypeRef, param_count);
			LLVMGetParamTypes(fnp, param_types);


			for (unsigned i = 0; i < param_count; i++) {
				LLVMTypeRef param_type = param_types[i];
				LLVMTypeRef arg_type = LLVMTypeOf(args[i]);
				if (LB_USE_NEW_PASS_SYSTEM &&
				    arg_type != param_type) {
					LLVMTypeKind arg_kind = LLVMGetTypeKind(arg_type);
					LLVMTypeKind param_kind = LLVMGetTypeKind(param_type);
					if (arg_kind == param_kind &&
					    arg_kind == LLVMPointerTypeKind) {
						// NOTE(bill): LLVM's newer `ptr` only type system seems to fail at times
						// I don't know why...
						args[i] = LLVMBuildPointerCast(p->builder, args[i], param_type, "");
						arg_type = param_type;
						continue;
					}
				}

				GB_ASSERT_MSG(
					arg_type == param_type,
					"Parameter types do not match: %s != %s, argument: %s\n\t%s",
					LLVMPrintTypeToString(arg_type),
					LLVMPrintTypeToString(param_type),
					LLVMPrintValueToString(args[i]),
					LLVMPrintTypeToString(fnp)
				);
			}
		}

		LLVMValueRef ret = LLVMBuildCall2(p->builder, fnp, fn, args, arg_count, "");

		auto llvm_cc = lb_calling_convention_map[proc_type->Proc.calling_convention];
		LLVMSetInstructionCallConv(ret, llvm_cc);

		LLVMAttributeIndex param_offset = LLVMAttributeIndex_FirstArgIndex;
		if (return_ptr.value != nullptr) {
			param_offset += 1;

			LLVMAddCallSiteAttribute(ret, 1, lb_create_enum_attribute_with_type(p->module->ctx, "sret", LLVMTypeOf(args[0])));
		}

		for_array(i, ft->args) {
			LLVMAttributeRef attribute = ft->args[i].attribute;
			if (attribute != nullptr) {
				LLVMAddCallSiteAttribute(ret, param_offset + cast(LLVMAttributeIndex)i, attribute);
			}
		}

		switch (inlining) {
		case ProcInlining_none:
			break;
		case ProcInlining_inline:
			LLVMAddCallSiteAttribute(ret, LLVMAttributeIndex_FunctionIndex, lb_create_enum_attribute(p->module->ctx, "alwaysinline"));
			break;
		case ProcInlining_no_inline:
			LLVMAddCallSiteAttribute(ret, LLVMAttributeIndex_FunctionIndex, lb_create_enum_attribute(p->module->ctx, "noinline"));
			break;
		}

		lbValue res = {};
		res.value = ret;
		res.type = abi_rt;
		return res;
	}
}


gb_internal lbValue lb_lookup_runtime_procedure(lbModule *m, String const &name) {
	AstPackage *pkg = m->info->runtime_package;
	Entity *e = scope_lookup_current(pkg->scope, name);
	return lb_find_procedure_value_from_entity(m, e);
}


gb_internal lbValue lb_emit_runtime_call(lbProcedure *p, char const *c_name, Array<lbValue> const &args) {
	String name = make_string_c(c_name);
	lbValue proc = lb_lookup_runtime_procedure(p->module, name);
	return lb_emit_call(p, proc, args);
}

gb_internal lbValue lb_emit_conjugate(lbProcedure *p, lbValue val, Type *type) {
	lbValue res = {};
	Type *t = val.type;
	if (is_type_complex(t)) {
		res = lb_addr_get_ptr(p, lb_add_local_generated(p, type, false));
		lbValue real = lb_emit_struct_ev(p, val, 0);
		lbValue imag = lb_emit_struct_ev(p, val, 1);
		imag = lb_emit_unary_arith(p, Token_Sub, imag, imag.type);
		lb_emit_store(p, lb_emit_struct_ep(p, res, 0), real);
		lb_emit_store(p, lb_emit_struct_ep(p, res, 1), imag);
	} else if (is_type_quaternion(t)) {
		// @QuaternionLayout
		res = lb_addr_get_ptr(p, lb_add_local_generated(p, type, false));
		lbValue real = lb_emit_struct_ev(p, val, 3);
		lbValue imag = lb_emit_struct_ev(p, val, 0);
		lbValue jmag = lb_emit_struct_ev(p, val, 1);
		lbValue kmag = lb_emit_struct_ev(p, val, 2);
		imag = lb_emit_unary_arith(p, Token_Sub, imag, imag.type);
		jmag = lb_emit_unary_arith(p, Token_Sub, jmag, jmag.type);
		kmag = lb_emit_unary_arith(p, Token_Sub, kmag, kmag.type);
		lb_emit_store(p, lb_emit_struct_ep(p, res, 3), real);
		lb_emit_store(p, lb_emit_struct_ep(p, res, 0), imag);
		lb_emit_store(p, lb_emit_struct_ep(p, res, 1), jmag);
		lb_emit_store(p, lb_emit_struct_ep(p, res, 2), kmag);
	} else if (is_type_array_like(t)) {
		res = lb_addr_get_ptr(p, lb_add_local_generated(p, type, true));
		Type *elem_type = base_array_type(t);
		i64 count = get_array_type_count(t);
		for (i64 i = 0; i < count; i++) {
			lbValue dst = lb_emit_array_epi(p, res, i);
			lbValue elem = lb_emit_struct_ev(p, val, cast(i32)i);
			elem = lb_emit_conjugate(p, elem, elem_type);
			lb_emit_store(p, dst, elem);
		}
	} else if (is_type_matrix(t)) {
		Type *mt = base_type(t);
		GB_ASSERT(mt->kind == Type_Matrix);
		Type *elem_type = mt->Matrix.elem;
		res = lb_addr_get_ptr(p, lb_add_local_generated(p, type, true));
		for (i64 j = 0; j < mt->Matrix.column_count; j++) {
			for (i64 i = 0; i < mt->Matrix.row_count; i++) {
				lbValue dst = lb_emit_matrix_epi(p, res, i, j);
				lbValue elem = lb_emit_matrix_ev(p, val, i, j);
				elem = lb_emit_conjugate(p, elem, elem_type);
				lb_emit_store(p, dst, elem);
			}
		}
	}
	return lb_emit_load(p, res);
}

gb_internal lbValue lb_emit_call(lbProcedure *p, lbValue value, Array<lbValue> const &args, ProcInlining inlining) {
	lbModule *m = p->module;

	Type *pt = base_type(value.type);
	GB_ASSERT(pt->kind == Type_Proc);
	Type *results = pt->Proc.results;

	lbAddr context_ptr = {};
	if (pt->Proc.calling_convention == ProcCC_Odin) {
		context_ptr = lb_find_or_generate_context_ptr(p);
	}

	defer (if (pt->Proc.diverging) {
		LLVMBuildUnreachable(p->builder);
	});

	bool is_c_vararg = pt->Proc.c_vararg;
	isize param_count = pt->Proc.param_count;
	if (is_c_vararg) {
		GB_ASSERT(param_count-1 <= args.count);
		param_count -= 1;
	} else {
		GB_ASSERT_MSG(param_count == args.count, "%td == %td (%s)", param_count, args.count, LLVMPrintValueToString(value.value));
	}

	lbValue result = {};

	isize ignored_args = 0;
	auto processed_args = array_make<lbValue>(permanent_allocator(), 0, args.count);

	{

		bool is_odin_cc = is_calling_convention_odin(pt->Proc.calling_convention);

		lbFunctionType *ft = lb_get_function_type(m, pt);
		bool return_by_pointer = ft->ret.kind == lbArg_Indirect;
		bool split_returns = ft->multiple_return_original_type != nullptr;

		unsigned param_index = 0;
		for (isize i = 0; i < param_count; i++) {
			Entity *e = pt->Proc.params->Tuple.variables[i];
			if (e->kind != Entity_Variable) {
				continue;
			}
			GB_ASSERT(e->flags & EntityFlag_Param);

			Type *original_type = e->type;
			lbArgType *arg = &ft->args[param_index];
			if (arg->kind == lbArg_Ignore) {
				param_index += 1;
				ignored_args += 1;
				continue;
			}

			lbValue x = lb_emit_conv(p, args[i], original_type);
			LLVMTypeRef xt = lb_type(p->module, x.type);

			if (arg->kind == lbArg_Direct) {
				LLVMTypeRef abi_type = arg->cast_type;
				if (!abi_type) {
					abi_type = arg->type;
				}
				if (xt == abi_type) {
					array_add(&processed_args, x);
				} else {
					x.value = OdinLLVMBuildTransmute(p, x.value, abi_type);
					array_add(&processed_args, x);
				}

			} else if (arg->kind == lbArg_Indirect) {
				lbValue ptr = {};
				if (arg->is_byval) {
					if (is_odin_cc) {
						if (are_types_identical(original_type, t_source_code_location)) {
							ptr = lb_address_from_load_or_generate_local(p, x);
						// } else {
						// 	ptr = lb_address_from_load_if_readonly_parameter(p, x);
						}
					}
					if (ptr.value == nullptr) {
						ptr = lb_copy_value_to_ptr(p, x, original_type, arg->byval_alignment);
					}
				} else if (is_odin_cc) {
					// NOTE(bill): Odin parameters are immutable so the original value can be passed if possible
					// i.e. `T const &` in C++
					if (LLVMIsConstant(x.value)) {
						// NOTE(bill): if the value is already constant, then just it as a global variable
						// and pass it by pointer
						lbAddr addr = lb_add_global_generated_from_procedure(p, original_type, x);
						lb_make_global_private_const(addr);
						ptr = addr.addr;
					} else {
						ptr = lb_address_from_load_or_generate_local(p, x);
					}
				} else {
					ptr = lb_copy_value_to_ptr(p, x, original_type, 16);
				}
				array_add(&processed_args, ptr);
			}

			param_index += 1;
		}

		if (is_c_vararg) {
			for (isize i = processed_args.count; i < args.count; i++) {
				array_add(&processed_args, args[i]);
			}
		}

		Type *rt = reduce_tuple_to_single_type(results);
		Type *original_rt = rt;
		if (split_returns) {
			GB_ASSERT(rt->kind == Type_Tuple);
			for (isize j = 0; j < rt->Tuple.variables.count-1; j++) {
				Type *partial_return_type = rt->Tuple.variables[j]->type;
				lbValue partial_return_ptr = lb_add_local(p, partial_return_type, nullptr, true, false).addr;
				array_add(&processed_args, partial_return_ptr);
			}
			rt = reduce_tuple_to_single_type(rt->Tuple.variables[rt->Tuple.variables.count-1]->type);
		}

		if (return_by_pointer) {
			lbValue return_ptr = lb_add_local_generated(p, rt, true).addr;
			lb_emit_call_internal(p, value, return_ptr, processed_args, nullptr, context_ptr, inlining);
			result = lb_emit_load(p, return_ptr);
		} else if (rt != nullptr) {
			result = lb_emit_call_internal(p, value, {}, processed_args, rt, context_ptr, inlining);
			if (ft->ret.cast_type) {
				result.value = OdinLLVMBuildTransmute(p, result.value, ft->ret.cast_type);
			}
			result.value = OdinLLVMBuildTransmute(p, result.value, ft->ret.type);
			result.type = rt;
			if (LLVMTypeOf(result.value) == LLVMInt1TypeInContext(p->module->ctx)) {
				result.type = t_llvm_bool;
			}
			if (!is_type_tuple(rt)) {
				result = lb_emit_conv(p, result, rt);
			}
		} else {
			lb_emit_call_internal(p, value, {}, processed_args, nullptr, context_ptr, inlining);
		}

		if (original_rt != rt) {
			GB_ASSERT(split_returns);
			GB_ASSERT(is_type_tuple(original_rt));

			// IMPORTANT NOTE(bill, 2022-11-24)
			// result_ptr is a dummy value which is only used to reference a tuple
			// value for the "tuple-fix"
			//
			// The reason for the fake stack allocation is to have a unique pointer
			// for the value to be used as a key within the procedure itself

			lbValue result_ptr = lb_add_local_generated(p, original_rt, false).addr;
			isize ret_count = original_rt->Tuple.variables.count;

			auto tuple_fix_values = slice_make<lbValue>(permanent_allocator(), ret_count);
			auto tuple_geps = slice_make<lbValue>(permanent_allocator(), ret_count);

			isize offset = ft->original_arg_count - ignored_args;
			for (isize j = 0; j < ret_count-1; j++) {
				lbValue ret_arg_ptr = processed_args[offset + j];
				lbValue ret_arg = lb_emit_load(p, ret_arg_ptr);
				tuple_fix_values[j] = ret_arg;
			}
			tuple_fix_values[ret_count-1] = result;

		#if 0
			for (isize j = 0; j < ret_count; j++) {
				tuple_geps[j] = lb_emit_struct_ep(p, result_ptr, cast(i32)j);
			}
			for (isize j = 0; j < ret_count; j++) {
				lb_emit_store(p, tuple_geps[j], tuple_fix_values[j]);
			}
		#endif

			result = lb_emit_load(p, result_ptr);

			lbTupleFix tf = {tuple_fix_values};
			map_set(&p->tuple_fix_map, result_ptr.value, tf);
			map_set(&p->tuple_fix_map, result.value, tf);
		}

	}

	LLVMValueRef the_proc_value = value.value;

	if (LLVMIsAConstantExpr(the_proc_value)) {
		// NOTE(bill): it's a bit cast
		the_proc_value = LLVMGetOperand(the_proc_value, 0);
	}
	Entity **found = map_get(&p->module->procedure_values, the_proc_value);
	if (found != nullptr) {
		Entity *e = *found;
		if (e != nullptr && entity_has_deferred_procedure(e)) {
			DeferredProcedureKind kind = e->Procedure.deferred_procedure.kind;
			Entity *deferred_entity = e->Procedure.deferred_procedure.entity;
			lbValue deferred = lb_find_procedure_value_from_entity(p->module, deferred_entity);


			bool by_ptr = false;
			auto in_args = args;
			Array<lbValue> result_as_args = {};
			switch (kind) {
			case DeferredProcedure_none:
				break;
			case DeferredProcedure_in_by_ptr:
				by_ptr = true;
				/*fallthrough*/
			case DeferredProcedure_in:
				result_as_args = array_clone(heap_allocator(), in_args);
				break;
			case DeferredProcedure_out_by_ptr:
				by_ptr = true;
				/*fallthrough*/
			case DeferredProcedure_out:
				result_as_args = lb_value_to_array(p, heap_allocator(), result);
				break;
			case DeferredProcedure_in_out_by_ptr:
				by_ptr = true;
				/*fallthrough*/
			case DeferredProcedure_in_out:
				{
					auto out_args = lb_value_to_array(p, heap_allocator(), result);
					array_init(&result_as_args, heap_allocator(), in_args.count + out_args.count);
					array_copy(&result_as_args, in_args, 0);
					array_copy(&result_as_args, out_args, in_args.count);
				}
				break;
			}
			if (by_ptr) {
				for_array(i, result_as_args) {
					lbValue arg_ptr = lb_address_from_load_or_generate_local(p, result_as_args[i]);
					result_as_args[i] = arg_ptr;
				}
			}

			lb_add_defer_proc(p, p->scope_index, deferred, result_as_args);
		}
	}

	return result;
}

gb_internal LLVMValueRef llvm_splat_int(i64 count, LLVMTypeRef type, i64 value, bool is_signed=false) {
	LLVMValueRef v = LLVMConstInt(type, value, is_signed);
	LLVMValueRef *values = gb_alloc_array(temporary_allocator(), LLVMValueRef, count);
	for (i64 i = 0; i < count; i++) {
		values[i] = v;
	}
	return LLVMConstVector(values, cast(unsigned)count);
}


gb_internal lbValue lb_build_builtin_simd_proc(lbProcedure *p, Ast *expr, TypeAndValue const &tv, BuiltinProcId builtin_id) {
	ast_node(ce, CallExpr, expr);

	lbModule *m = p->module;

	lbValue res = {};
	res.type = tv.type;

	switch (builtin_id) {
	case BuiltinProc_simd_indices: {
		Type *type = base_type(res.type);
		GB_ASSERT(type->kind == Type_SimdVector);
		Type *elem = type->SimdVector.elem;

		i64 count = type->SimdVector.count;
		LLVMValueRef *scalars = gb_alloc_array(temporary_allocator(), LLVMValueRef, count);
		for (i64 i = 0; i < count; i++) {
			scalars[i] = lb_const_value(m, elem, exact_value_i64(i)).value;
		}

		res.value = LLVMConstVector(scalars, cast(unsigned)count);
		return res;
	}
	}

	lbValue arg0 = {}; if (ce->args.count > 0) arg0 = lb_build_expr(p, ce->args[0]);
	lbValue arg1 = {}; if (ce->args.count > 1) arg1 = lb_build_expr(p, ce->args[1]);
	lbValue arg2 = {}; if (ce->args.count > 2) arg2 = lb_build_expr(p, ce->args[2]);

	Type *elem = base_array_type(arg0.type);

	bool is_float = is_type_float(elem);
	bool is_signed = !is_type_unsigned(elem);

	LLVMOpcode op_code = cast(LLVMOpcode)0;

	switch (builtin_id) {
	case BuiltinProc_simd_add:
	case BuiltinProc_simd_sub:
	case BuiltinProc_simd_mul:
	case BuiltinProc_simd_div:
	case BuiltinProc_simd_rem:
		if (is_float) {
			switch (builtin_id) {
			case BuiltinProc_simd_add: op_code = LLVMFAdd; break;
			case BuiltinProc_simd_sub: op_code = LLVMFSub; break;
			case BuiltinProc_simd_mul: op_code = LLVMFMul; break;
			case BuiltinProc_simd_div: op_code = LLVMFDiv; break;
			}
		} else {
			switch (builtin_id) {
			case BuiltinProc_simd_add: op_code = LLVMAdd; break;
			case BuiltinProc_simd_sub: op_code = LLVMSub; break;
			case BuiltinProc_simd_mul: op_code = LLVMMul; break;
			case BuiltinProc_simd_div:
				if (is_signed) {
					op_code = LLVMSDiv;
				} else {
					op_code = LLVMUDiv;
				}
				break;
			case BuiltinProc_simd_rem:
				if (is_signed) {
					op_code = LLVMSRem;
				} else {
					op_code = LLVMURem;
				}
				break;
			}
		}
		if (op_code) {
			res.value = LLVMBuildBinOp(p->builder, op_code, arg0.value, arg1.value, "");
			return res;
		}
		break;
	case BuiltinProc_simd_shl: // Odin logic
	case BuiltinProc_simd_shr: // Odin logic
	case BuiltinProc_simd_shl_masked: // C logic
	case BuiltinProc_simd_shr_masked: // C logic
		{
			i64 sz = type_size_of(elem);
			GB_ASSERT(arg0.type->kind == Type_SimdVector);

			i64 count = arg0.type->SimdVector.count;
			Type *elem1 = base_array_type(arg1.type);

			bool is_masked = false;
			switch (builtin_id) {
			case BuiltinProc_simd_shl:        op_code = LLVMShl;                         is_masked = false; break;
			case BuiltinProc_simd_shr:        op_code = is_signed ? LLVMAShr : LLVMLShr; is_masked = false; break;
			case BuiltinProc_simd_shl_masked: op_code = LLVMShl;                         is_masked = true;  break;
			case BuiltinProc_simd_shr_masked: op_code = is_signed ? LLVMAShr : LLVMLShr; is_masked = true;  break;
			}
			if (op_code) {
				LLVMValueRef bits = llvm_splat_int(count, lb_type(m, elem1), sz*8 - 1);
				if (is_masked) {
					// C logic
					LLVMValueRef shift = LLVMBuildAnd(p->builder, arg1.value, bits, "");
					res.value = LLVMBuildBinOp(p->builder, op_code, arg0.value, shift, "");
				} else {
					// Odin logic
					LLVMValueRef zero = lb_const_nil(m, arg1.type).value;
					LLVMValueRef mask = LLVMBuildICmp(p->builder, LLVMIntULE, arg1.value, bits, "");
					LLVMValueRef shift = LLVMBuildBinOp(p->builder, op_code, arg0.value, arg1.value, "");
					res.value = LLVMBuildSelect(p->builder, mask, shift, zero, "");
				}
				return res;
			}
		}
		break;
	case BuiltinProc_simd_bit_and:
	case BuiltinProc_simd_bit_or:
	case BuiltinProc_simd_bit_xor:
	case BuiltinProc_simd_bit_and_not:
		switch (builtin_id) {
		case BuiltinProc_simd_bit_and: op_code = LLVMAnd; break;
		case BuiltinProc_simd_bit_or:  op_code = LLVMOr;  break;
		case BuiltinProc_simd_bit_xor: op_code = LLVMXor; break;
		case BuiltinProc_simd_bit_and_not:
			op_code = LLVMAnd;
			arg1.value = LLVMBuildNot(p->builder, arg1.value, "");
			break;
		}
		if (op_code) {
			res.value = LLVMBuildBinOp(p->builder, op_code, arg0.value, arg1.value, "");
			return res;
		}
		break;
	case BuiltinProc_simd_neg:
		if (is_float) {
			res.value = LLVMBuildFNeg(p->builder, arg0.value, "");
		} else {
			res.value = LLVMBuildNeg(p->builder, arg0.value, "");
		}
		return res;
	case BuiltinProc_simd_abs:
		if (is_float) {
			LLVMValueRef pos = arg0.value;
			LLVMValueRef neg = LLVMBuildFNeg(p->builder, pos, "");
			LLVMValueRef cond = LLVMBuildFCmp(p->builder, LLVMRealOGT, pos, neg, "");
			res.value = LLVMBuildSelect(p->builder, cond, pos, neg, "");
		} else {
			LLVMValueRef pos = arg0.value;
			LLVMValueRef neg = LLVMBuildNeg(p->builder, pos, "");
			LLVMValueRef cond = LLVMBuildICmp(p->builder, is_signed ? LLVMIntSGT : LLVMIntUGT, pos, neg, "");
			res.value = LLVMBuildSelect(p->builder, cond, pos, neg, "");
		}
		return res;
	case BuiltinProc_simd_min:
		if (is_float) {
			return lb_emit_min(p, res.type, arg0, arg1);
		} else {
			LLVMValueRef cond = LLVMBuildICmp(p->builder, is_signed ? LLVMIntSLT : LLVMIntULT, arg0.value, arg1.value, "");
			res.value = LLVMBuildSelect(p->builder, cond, arg0.value, arg1.value, "");
		}
		return res;
	case BuiltinProc_simd_max:
		if (is_float) {
			return lb_emit_max(p, res.type, arg0, arg1);
		} else {
			LLVMValueRef cond = LLVMBuildICmp(p->builder, is_signed ? LLVMIntSGT : LLVMIntUGT, arg0.value, arg1.value, "");
			res.value = LLVMBuildSelect(p->builder, cond, arg0.value, arg1.value, "");
		}
		return res;
	case BuiltinProc_simd_lanes_eq:
	case BuiltinProc_simd_lanes_ne:
	case BuiltinProc_simd_lanes_lt:
	case BuiltinProc_simd_lanes_le:
	case BuiltinProc_simd_lanes_gt:
	case BuiltinProc_simd_lanes_ge:
		if (is_float) {
			LLVMRealPredicate pred = cast(LLVMRealPredicate)0;
			switch (builtin_id) {
			case BuiltinProc_simd_lanes_eq: pred = LLVMRealOEQ; break;
			case BuiltinProc_simd_lanes_ne: pred = LLVMRealUNE; break;
			case BuiltinProc_simd_lanes_lt: pred = LLVMRealOLT; break;
			case BuiltinProc_simd_lanes_le: pred = LLVMRealOLE; break;
			case BuiltinProc_simd_lanes_gt: pred = LLVMRealOGT; break;
			case BuiltinProc_simd_lanes_ge: pred = LLVMRealOGE; break;
			}
			if (pred) {
				res.value = LLVMBuildFCmp(p->builder, pred, arg0.value, arg1.value, "");
				res.value = LLVMBuildSExtOrBitCast(p->builder, res.value, lb_type(m, tv.type), "");
				return res;
			}
		} else {
			LLVMIntPredicate pred = cast(LLVMIntPredicate)0;
			switch (builtin_id) {
			case BuiltinProc_simd_lanes_eq: pred = LLVMIntEQ; break;
			case BuiltinProc_simd_lanes_ne: pred = LLVMIntNE; break;
			case BuiltinProc_simd_lanes_lt: pred = is_signed ? LLVMIntSLT :LLVMIntULT; break;
			case BuiltinProc_simd_lanes_le: pred = is_signed ? LLVMIntSLE :LLVMIntULE; break;
			case BuiltinProc_simd_lanes_gt: pred = is_signed ? LLVMIntSGT :LLVMIntUGT; break;
			case BuiltinProc_simd_lanes_ge: pred = is_signed ? LLVMIntSGE :LLVMIntUGE; break;
			}
			if (pred) {
				res.value = LLVMBuildICmp(p->builder, pred, arg0.value, arg1.value, "");
				res.value = LLVMBuildSExtOrBitCast(p->builder, res.value, lb_type(m, tv.type), "");
				return res;
			}
		}
		break;

	case BuiltinProc_simd_extract:
		res.value = LLVMBuildExtractElement(p->builder, arg0.value, arg1.value, "");
		return res;
	case BuiltinProc_simd_replace:
		res.value = LLVMBuildInsertElement(p->builder, arg0.value, arg2.value, arg1.value, "");
		return res;

	case BuiltinProc_simd_reduce_add_bisect:
	case BuiltinProc_simd_reduce_mul_bisect:
		{
			GB_ASSERT(arg0.type->kind == Type_SimdVector);
			i64 num_elems = arg0.type->SimdVector.count;

			LLVMValueRef *indices = gb_alloc_array(temporary_allocator(), LLVMValueRef, num_elems);
			for (i64 i = 0; i < num_elems; i++) {
				indices[i] = lb_const_int(m, t_uint, cast(u64)i).value;
			}

			switch (builtin_id) {
			case BuiltinProc_simd_reduce_add_bisect: op_code = is_float ? LLVMFAdd : LLVMAdd; break;
			case BuiltinProc_simd_reduce_mul_bisect: op_code = is_float ? LLVMFMul : LLVMMul; break;
			}

			LLVMValueRef remaining = arg0.value;
			i64 num_remaining = num_elems;

			while (num_remaining > 1) {
				num_remaining /= 2;
				LLVMValueRef left_indices = LLVMConstVector(&indices[0], cast(unsigned)num_remaining);
				LLVMValueRef left_value = LLVMBuildShuffleVector(p->builder, remaining, remaining, left_indices, "");
				LLVMValueRef right_indices = LLVMConstVector(&indices[num_remaining], cast(unsigned)num_remaining);
				LLVMValueRef right_value = LLVMBuildShuffleVector(p->builder, remaining, remaining, right_indices, "");
				remaining = LLVMBuildBinOp(p->builder, op_code, left_value, right_value, "");
			}

			res.value = LLVMBuildExtractElement(p->builder, remaining, indices[0], "");
			return res;
		}

	case BuiltinProc_simd_reduce_add_ordered:
	case BuiltinProc_simd_reduce_mul_ordered:
		{
			LLVMTypeRef llvm_elem = lb_type(m, elem);
			LLVMValueRef args[2] = {};
			isize args_count = 0;

			char const *name = nullptr;
			switch (builtin_id) {
			case BuiltinProc_simd_reduce_add_ordered:
				if (is_float) {
					name = "llvm.vector.reduce.fadd";
					args[args_count++] = LLVMConstReal(llvm_elem, 0.0);
				} else {
					name = "llvm.vector.reduce.add";
				}
				break;
			case BuiltinProc_simd_reduce_mul_ordered:
				if (is_float) {
					name = "llvm.vector.reduce.fmul";
					args[args_count++] = LLVMConstReal(llvm_elem, 1.0);
				} else {
					name = "llvm.vector.reduce.mul";
				}
				break;
			}
			args[args_count++] = arg0.value;

			LLVMTypeRef types[1] = {lb_type(p->module, arg0.type)};
			res.value = lb_call_intrinsic(p, name, args, cast(unsigned)args_count, types, gb_count_of(types));
			return res;
		}

	case BuiltinProc_simd_reduce_add_pairs:
	case BuiltinProc_simd_reduce_mul_pairs:
		{
			GB_ASSERT(arg0.type->kind == Type_SimdVector);
			i64 num_elems = arg0.type->SimdVector.count;

			LLVMValueRef *indices = gb_alloc_array(temporary_allocator(), LLVMValueRef, num_elems);
			for (i64 i = 0; i < num_elems/2; i++) {
				indices[i] = lb_const_int(m, t_uint, cast(u64)(2*i)).value;
				indices[i+num_elems/2] = lb_const_int(m, t_uint, cast(u64)(2*i+1)).value;
			}

			switch (builtin_id) {
			case BuiltinProc_simd_reduce_add_pairs: op_code = is_float ? LLVMFAdd : LLVMAdd; break;
			case BuiltinProc_simd_reduce_mul_pairs: op_code = is_float ? LLVMFMul : LLVMMul; break;
			}

			LLVMValueRef remaining = arg0.value;
			i64 num_remaining = num_elems;

			while (num_remaining > 1) {
				num_remaining /= 2;
				LLVMValueRef left_indices = LLVMConstVector(&indices[0], cast(unsigned)num_remaining);
				LLVMValueRef left_value = LLVMBuildShuffleVector(p->builder, remaining, remaining, left_indices, "");
				LLVMValueRef right_indices = LLVMConstVector(&indices[num_elems/2], cast(unsigned)num_remaining);
				LLVMValueRef right_value = LLVMBuildShuffleVector(p->builder, remaining, remaining, right_indices, "");
				remaining = LLVMBuildBinOp(p->builder, op_code, left_value, right_value, "");
			}

			res.value = LLVMBuildExtractElement(p->builder, remaining, indices[0], "");
			return res;
		}

	case BuiltinProc_simd_reduce_min:
	case BuiltinProc_simd_reduce_max:
	case BuiltinProc_simd_reduce_and:
	case BuiltinProc_simd_reduce_or:
	case BuiltinProc_simd_reduce_xor:
		{
			char const *name = nullptr;
			switch (builtin_id) {
			case BuiltinProc_simd_reduce_min:
				if (is_float) {
					name = "llvm.vector.reduce.fmin";
				} else if (is_signed) {
					name = "llvm.vector.reduce.smin";
				} else {
					name = "llvm.vector.reduce.umin";
				}
				break;
			case BuiltinProc_simd_reduce_max:
				if (is_float) {
					name = "llvm.vector.reduce.fmax";
				} else if (is_signed) {
					name = "llvm.vector.reduce.smax";
				} else {
					name = "llvm.vector.reduce.umax";
				}
				break;
			case BuiltinProc_simd_reduce_and: name = "llvm.vector.reduce.and"; break;
			case BuiltinProc_simd_reduce_or:  name = "llvm.vector.reduce.or";  break;
			case BuiltinProc_simd_reduce_xor: name = "llvm.vector.reduce.xor"; break;
			}

			LLVMTypeRef types[1] = { lb_type(p->module, arg0.type) };
			LLVMValueRef args[1] = { arg0.value };

			res.value = lb_call_intrinsic(p, name, args, gb_count_of(args), types, gb_count_of(types));
			return res;
		}

	case BuiltinProc_simd_reduce_any:
	case BuiltinProc_simd_reduce_all:
		{
			char const *name = nullptr;
			switch (builtin_id) {
			case BuiltinProc_simd_reduce_any: name = "llvm.vector.reduce.or";  break;
			case BuiltinProc_simd_reduce_all: name = "llvm.vector.reduce.and"; break;
			}

			LLVMTypeRef types[1] = { lb_type(p->module, arg0.type) };
			LLVMValueRef args[1] = { arg0.value };

			res.value = lb_call_intrinsic(p, name, args, gb_count_of(args), types, gb_count_of(types));
			return res;
		}

	case BuiltinProc_simd_extract_lsbs:
	case BuiltinProc_simd_extract_msbs:
		{
			Type *vt = arg0.type;
			GB_ASSERT(vt->kind == Type_SimdVector);

			i64 elem_bits = 8*type_size_of(elem);
			i64 num_elems = get_array_type_count(vt);

			LLVMValueRef broadcast_value = arg0.value;
			if (builtin_id == BuiltinProc_simd_extract_msbs) {
				LLVMTypeRef word_type = lb_type(m, elem);
				LLVMValueRef shift_value = llvm_splat_int(num_elems, word_type, elem_bits - 1);
				broadcast_value = LLVMBuildAShr(p->builder, broadcast_value, shift_value, "");
			}

			LLVMTypeRef bitvec_type = LLVMVectorType(LLVMInt1TypeInContext(m->ctx), (unsigned)num_elems);
			LLVMValueRef bitvec_value = LLVMBuildTrunc(p->builder, broadcast_value, bitvec_type, "");

			LLVMTypeRef mask_type = LLVMIntTypeInContext(m->ctx, (unsigned)num_elems);
			LLVMValueRef mask_value = LLVMBuildBitCast(p->builder, bitvec_value, mask_type, "");

			LLVMTypeRef result_type = lb_type(m, res.type);
			res.value = LLVMBuildZExtOrBitCast(p->builder, mask_value, result_type, "");

			return res;
		}


	case BuiltinProc_simd_shuffle:
		{
			Type *vt = arg0.type;
			GB_ASSERT(vt->kind == Type_SimdVector);

			i64 indices_count = ce->args.count-2;
			i64 max_count = vt->SimdVector.count*2;
			GB_ASSERT(indices_count <= max_count);

			LLVMValueRef *values = gb_alloc_array(temporary_allocator(), LLVMValueRef, indices_count);
			for (isize i = 0; i < indices_count; i++) {
				lbValue idx = lb_build_expr(p, ce->args[i+2]);
				GB_ASSERT(LLVMIsConstant(idx.value));
				values[i] = idx.value;
			}
			LLVMValueRef indices = LLVMConstVector(values, cast(unsigned)indices_count);

			res.value = LLVMBuildShuffleVector(p->builder, arg0.value, arg1.value, indices, "");
			return res;
		}

	case BuiltinProc_simd_select:
		{
			LLVMValueRef cond = arg0.value;
			LLVMValueRef x = lb_build_expr(p, ce->args[1]).value;
			LLVMValueRef y = lb_build_expr(p, ce->args[2]).value;

			cond = LLVMBuildICmp(p->builder, LLVMIntNE, cond, LLVMConstNull(LLVMTypeOf(cond)), "");
			res.value = LLVMBuildSelect(p->builder, cond, x, y, "");
			return res;
		}

	case BuiltinProc_simd_runtime_swizzle:
		{
			LLVMValueRef src = arg0.value;
			LLVMValueRef indices = lb_build_expr(p, ce->args[1]).value;
			
			Type *vt = arg0.type;
			GB_ASSERT(vt->kind == Type_SimdVector);
			i64 count = vt->SimdVector.count;
			Type *elem_type = vt->SimdVector.elem;
			i64 elem_size = type_size_of(elem_type);
			
			// Determine strategy based on element size and target architecture
			char const *intrinsic_name = nullptr;
			bool use_hardware_runtime_swizzle = false;
			
			// 8-bit elements: Use dedicated table lookup instructions
			if (elem_size == 1) {
				use_hardware_runtime_swizzle = true;
				
				if (build_context.metrics.arch == TargetArch_amd64 || build_context.metrics.arch == TargetArch_i386) {
					// x86/x86-64: Use pshufb intrinsics
					switch (count) {
					case 16:
						intrinsic_name = "llvm.x86.ssse3.pshuf.b.128";
						break;
					case 32:
						intrinsic_name = "llvm.x86.avx2.pshuf.b";
						break;
					case 64:
						intrinsic_name = "llvm.x86.avx512.pshuf.b.512";
						break;
					default:
						use_hardware_runtime_swizzle = false;
						break;
					}
				} else if (build_context.metrics.arch == TargetArch_arm64) {
					// ARM64: Use NEON tbl intrinsics with automatic table splitting
					switch (count) {
					case 16:
						intrinsic_name = "llvm.aarch64.neon.tbl1";
						break;
					case 32:
						intrinsic_name = "llvm.aarch64.neon.tbl2";
						break;
					case 48:
						intrinsic_name = "llvm.aarch64.neon.tbl3";
						break;
					case 64:
						intrinsic_name = "llvm.aarch64.neon.tbl4";
						break;
					default:
						use_hardware_runtime_swizzle = false;
						break;
					}
				} else if (build_context.metrics.arch == TargetArch_arm32) {
					// ARM32: Use NEON vtbl intrinsics with automatic table splitting
					switch (count) {
					case 8:
						intrinsic_name = "llvm.arm.neon.vtbl1";
						break;
					case 16:
						intrinsic_name = "llvm.arm.neon.vtbl2";
						break;
					case 24:
						intrinsic_name = "llvm.arm.neon.vtbl3";
						break;
					case 32:
						intrinsic_name = "llvm.arm.neon.vtbl4";
						break;
					default:
						use_hardware_runtime_swizzle = false;
						break;
					}
				} else if (build_context.metrics.arch == TargetArch_wasm32 || build_context.metrics.arch == TargetArch_wasm64p32) {
					// WebAssembly: Use swizzle (only supports 16-byte vectors)
					if (count == 16) {
						intrinsic_name = "llvm.wasm.swizzle";
					} else {
						use_hardware_runtime_swizzle = false;
					}
				} else {
					use_hardware_runtime_swizzle = false;
				}
			}
			
			if (use_hardware_runtime_swizzle && intrinsic_name != nullptr) {
				// Use dedicated hardware swizzle instruction
				
				// Check if required target features are enabled
				bool features_enabled = true;
				if (build_context.metrics.arch == TargetArch_amd64 || build_context.metrics.arch == TargetArch_i386) {
					// x86/x86-64 feature checking
					if (count == 16) {
						// SSE/SSSE3 for 128-bit vectors
						if (!check_target_feature_is_enabled(str_lit("ssse3"), nullptr)) {
							features_enabled = false;
						}
					} else if (count == 32) {
						// AVX2 requires ssse3 + avx2 features
						if (!check_target_feature_is_enabled(str_lit("ssse3"), nullptr) || 
							!check_target_feature_is_enabled(str_lit("avx2"), nullptr)) {
							features_enabled = false;
						}
					} else if (count == 64) {
						// AVX512 requires ssse3 + avx2 + avx512f + avx512bw features
						if (!check_target_feature_is_enabled(str_lit("ssse3"), nullptr) ||
							!check_target_feature_is_enabled(str_lit("avx2"), nullptr) ||
							!check_target_feature_is_enabled(str_lit("avx512f"), nullptr) ||
							!check_target_feature_is_enabled(str_lit("avx512bw"), nullptr)) {
							features_enabled = false;
						}
					}
				} else if (build_context.metrics.arch == TargetArch_arm64 || build_context.metrics.arch == TargetArch_arm32) {
					// ARM/ARM64 feature checking - NEON is required for all table/swizzle ops
					if (!check_target_feature_is_enabled(str_lit("neon"), nullptr)) {
						features_enabled = false;
					}
				}
				
				if (features_enabled) {
					// Add target features to function attributes for LLVM instruction selection
					if (build_context.metrics.arch == TargetArch_amd64 || build_context.metrics.arch == TargetArch_i386) {
						// x86/x86-64 function attributes
						if (count == 16) {
							// SSE/SSSE3 for 128-bit vectors
							lb_add_attribute_to_proc_with_string(p->module, p->value, str_lit("target-features"), str_lit("+ssse3"));
							lb_add_attribute_to_proc_with_string(p->module, p->value, str_lit("min-legal-vector-width"), str_lit("128"));
						} else if (count == 32) {
							lb_add_attribute_to_proc_with_string(p->module, p->value, str_lit("target-features"), str_lit("+avx,+avx2,+ssse3"));
							lb_add_attribute_to_proc_with_string(p->module, p->value, str_lit("min-legal-vector-width"), str_lit("256"));
						} else if (count == 64) {
							lb_add_attribute_to_proc_with_string(p->module, p->value, str_lit("target-features"), str_lit("+avx,+avx2,+avx512f,+avx512bw,+ssse3"));
							lb_add_attribute_to_proc_with_string(p->module, p->value, str_lit("min-legal-vector-width"), str_lit("512"));
						}
					} else if (build_context.metrics.arch == TargetArch_arm64) {
						// ARM64 function attributes - enable NEON for swizzle instructions
						lb_add_attribute_to_proc_with_string(p->module, p->value, str_lit("target-features"), str_lit("+neon"));
						// Set appropriate vector width for multi-swizzle operations
						if (count >= 32) {
							lb_add_attribute_to_proc_with_string(p->module, p->value, str_lit("min-legal-vector-width"), str_lit("256"));
						}
					} else if (build_context.metrics.arch == TargetArch_arm32) {
						// ARM32 function attributes - enable NEON for swizzle instructions
						lb_add_attribute_to_proc_with_string(p->module, p->value, str_lit("target-features"), str_lit("+neon"));
					}
					
					// Handle ARM's multi-swizzle intrinsics by splitting the src vector
					if (build_context.metrics.arch == TargetArch_arm64 && count > 16) {
						// ARM64 TBL2/TBL3/TBL4: Split src into multiple 16-byte vectors
						int num_tables = cast(int)(count / 16);
						GB_ASSERT_MSG(count % 16 == 0, "ARM64 src size must be multiple of 16 bytes, got %lld bytes", count);
						GB_ASSERT_MSG(num_tables <= 4, "ARM64 NEON supports maximum 4 tables (tbl4), got %d tables for %lld-byte vector", num_tables, count);
						
						LLVMValueRef src_parts[4]; // Max 4 tables for tbl4
						for (int i = 0; i < num_tables; i++) {
							// Extract 16-byte slice from the larger src
							LLVMValueRef indices_for_extract[16];
							for (int j = 0; j < 16; j++) {
								indices_for_extract[j] = LLVMConstInt(LLVMInt32TypeInContext(p->module->ctx), i * 16 + j, false);
							}
							LLVMValueRef extract_mask = LLVMConstVector(indices_for_extract, 16);
							src_parts[i] = LLVMBuildShuffleVector(p->builder, src, LLVMGetUndef(LLVMTypeOf(src)), extract_mask, "");
						}
						
						// Call appropriate ARM64 tbl intrinsic
						if (count == 32) {
							LLVMValueRef args[3] = { src_parts[0], src_parts[1], indices };
							res.value = lb_call_intrinsic(p, intrinsic_name, args, 3, nullptr, 0);
						} else if (count == 48) {
							LLVMValueRef args[4] = { src_parts[0], src_parts[1], src_parts[2], indices };
							res.value = lb_call_intrinsic(p, intrinsic_name, args, 4, nullptr, 0);
						} else if (count == 64) {
							LLVMValueRef args[5] = { src_parts[0], src_parts[1], src_parts[2], src_parts[3], indices };
							res.value = lb_call_intrinsic(p, intrinsic_name, args, 5, nullptr, 0);
						}
					} else if (build_context.metrics.arch == TargetArch_arm32 && count > 8) {
						// ARM32 VTBL2/VTBL3/VTBL4: Split src into multiple 8-byte vectors
						int num_tables = cast(int)count / 8;
						GB_ASSERT_MSG(count % 8 == 0, "ARM32 src size must be multiple of 8 bytes, got %lld bytes", count);
						GB_ASSERT_MSG(num_tables <= 4, "ARM32 NEON supports maximum 4 tables (vtbl4), got %d tables for %lld-byte vector", num_tables, count);
						
						LLVMValueRef src_parts[4]; // Max 4 tables for vtbl4
						for (int i = 0; i < num_tables; i++) {
							// Extract 8-byte slice from the larger src
							LLVMValueRef indices_for_extract[8];
							for (int j = 0; j < 8; j++) {
								indices_for_extract[j] = LLVMConstInt(LLVMInt32TypeInContext(p->module->ctx), i * 8 + j, false);
							}
							LLVMValueRef extract_mask = LLVMConstVector(indices_for_extract, 8);
							src_parts[i] = LLVMBuildShuffleVector(p->builder, src, LLVMGetUndef(LLVMTypeOf(src)), extract_mask, "");
						}
						
						// Call appropriate ARM32 vtbl intrinsic
						if (count == 16) {
							LLVMValueRef args[3] = { src_parts[0], src_parts[1], indices };
							res.value = lb_call_intrinsic(p, intrinsic_name, args, 3, nullptr, 0);
						} else if (count == 24) {
							LLVMValueRef args[4] = { src_parts[0], src_parts[1], src_parts[2], indices };
							res.value = lb_call_intrinsic(p, intrinsic_name, args, 4, nullptr, 0);
						} else if (count == 32) {
							LLVMValueRef args[5] = { src_parts[0], src_parts[1], src_parts[2], src_parts[3], indices };
							res.value = lb_call_intrinsic(p, intrinsic_name, args, 5, nullptr, 0);
						}
					} else {
						// Single runtime swizzle case (x86, WebAssembly, ARM single-table)
						LLVMValueRef args[2] = { src, indices };
						res.value = lb_call_intrinsic(p, intrinsic_name, args, gb_count_of(args), nullptr, 0);
					}
					return res;
				} else {
					// Features not enabled, fall back to emulation
					use_hardware_runtime_swizzle = false;
				}
			}
			
			// Fallback: Emulate with extracts and inserts for all element sizes
			GB_ASSERT(count > 0 && count <= 64); // Sanity check
			
			LLVMValueRef *values = gb_alloc_array(temporary_allocator(), LLVMValueRef, count);
			LLVMTypeRef i32_type = LLVMInt32TypeInContext(p->module->ctx);
			LLVMTypeRef elem_llvm_type = lb_type(p->module, elem_type);
			
			// Calculate mask based on element size and vector count
			i64 max_index = count - 1;
			LLVMValueRef index_mask;
			
			if (elem_size == 1) {
				// 8-bit: mask to src size (like pshufb behavior)
				index_mask = LLVMConstInt(elem_llvm_type, max_index, false);
			} else if (elem_size == 2) {
				// 16-bit: mask to src size 
				index_mask = LLVMConstInt(elem_llvm_type, max_index, false);
			} else if (elem_size == 4) {
				// 32-bit: mask to src size
				index_mask = LLVMConstInt(elem_llvm_type, max_index, false);
			} else {
				// 64-bit: mask to src size
				index_mask = LLVMConstInt(elem_llvm_type, max_index, false);
			}
			
			for (i64 i = 0; i < count; i++) {
				LLVMValueRef idx_i = LLVMConstInt(i32_type, cast(unsigned)i, false);
				LLVMValueRef index_elem = LLVMBuildExtractElement(p->builder, indices, idx_i, "");
				
				// Mask index to valid range
				LLVMValueRef masked_index = LLVMBuildAnd(p->builder, index_elem, index_mask, "");
				
				// Convert to i32 for extractelement
				LLVMValueRef index_i32;
				if (LLVMGetIntTypeWidth(LLVMTypeOf(masked_index)) < 32) {
					index_i32 = LLVMBuildZExt(p->builder, masked_index, i32_type, "");
				} else if (LLVMGetIntTypeWidth(LLVMTypeOf(masked_index)) > 32) {
					index_i32 = LLVMBuildTrunc(p->builder, masked_index, i32_type, "");
				} else {
					index_i32 = masked_index;
				}
				
				values[i] = LLVMBuildExtractElement(p->builder, src, index_i32, "");
			}
			
			// Build result vector
			res.value = LLVMGetUndef(LLVMTypeOf(src));
			for (i64 i = 0; i < count; i++) {
				LLVMValueRef idx_i = LLVMConstInt(i32_type, cast(unsigned)i, false);
				res.value = LLVMBuildInsertElement(p->builder, res.value, values[i], idx_i, "");
			}
			return res;
		}

	case BuiltinProc_simd_ceil:
	case BuiltinProc_simd_floor:
	case BuiltinProc_simd_trunc:
	case BuiltinProc_simd_nearest:
		{
			char const *name = nullptr;
			switch (builtin_id) {
			case BuiltinProc_simd_ceil:    name = "llvm.ceil"; break;
			case BuiltinProc_simd_floor:   name = "llvm.floor"; break;
			case BuiltinProc_simd_trunc:   name = "llvm.trunc"; break;
			case BuiltinProc_simd_nearest: name = "llvm.nearbyint"; break;
			}

			LLVMTypeRef types[1] = { lb_type(p->module, arg0.type) };
			LLVMValueRef args[1] = { arg0.value };

			res.value = lb_call_intrinsic(p, name, args, gb_count_of(args), types, gb_count_of(types));
			return res;
		}

	case BuiltinProc_simd_lanes_reverse:
		{
			i64 count = get_array_type_count(arg0.type);
			LLVMValueRef *values = gb_alloc_array(temporary_allocator(), LLVMValueRef, count);
			LLVMTypeRef llvm_u32 = lb_type(m, t_u32);
			for (i64 i = 0; i < count; i++) {
				values[i] = LLVMConstInt(llvm_u32, count-1-i, false);
			}
			LLVMValueRef mask = LLVMConstVector(values, cast(unsigned)count);

			LLVMValueRef v = arg0.value;
			res.value = LLVMBuildShuffleVector(p->builder, v, v, mask, "");
			return res;
		}

	case BuiltinProc_simd_lanes_rotate_left:
	case BuiltinProc_simd_lanes_rotate_right:
		{

			i64 count = get_array_type_count(arg0.type);
			GB_ASSERT(is_power_of_two(count));
			BigInt bi_count = {};
			big_int_from_i64(&bi_count, count);

			TypeAndValue const &tv = ce->args[1]->tav;
			ExactValue val = exact_value_to_integer(tv.value);
			GB_ASSERT(val.kind == ExactValue_Integer);
			BigInt *bi = &val.value_integer;
			if (builtin_id == BuiltinProc_simd_lanes_rotate_right) {
				big_int_neg(bi, bi);
			}
			big_int_rem(bi, bi, &bi_count);
			big_int_dealloc(&bi_count);

			i64 left = big_int_to_i64(bi);

			LLVMValueRef *values = gb_alloc_array(temporary_allocator(), LLVMValueRef, count);
			LLVMTypeRef llvm_u32 = lb_type(m, t_u32);
			for (i64 i = 0; i < count; i++) {
				u64 idx = cast(u64)(i+left) & cast(u64)(count-1);
				values[i] = LLVMConstInt(llvm_u32, idx, false);
			}
			LLVMValueRef mask = LLVMConstVector(values, cast(unsigned)count);

			LLVMValueRef v = arg0.value;
			res.value = LLVMBuildShuffleVector(p->builder, v, v, mask, "");
			return res;
		}


	case BuiltinProc_simd_saturating_add:
	case BuiltinProc_simd_saturating_sub:
		{
			char const *name = nullptr;
			switch (builtin_id) {
			case BuiltinProc_simd_saturating_add: name = is_signed ? "llvm.sadd.sat" : "llvm.uadd.sat"; break;
			case BuiltinProc_simd_saturating_sub: name = is_signed ? "llvm.ssub.sat" : "llvm.usub.sat"; break;
			}

			LLVMTypeRef types[1] = {lb_type(p->module, arg0.type)};

			LLVMValueRef args[2] = { arg0.value, arg1.value };

			res.value = lb_call_intrinsic(p, name, args, gb_count_of(args), types, gb_count_of(types));
			return res;
		}

	case BuiltinProc_simd_clamp:
		{
			LLVMValueRef v = arg0.value;
			LLVMValueRef min = arg1.value;
			LLVMValueRef max = arg2.value;

			if (is_float) {
				v = LLVMBuildSelect(p->builder, LLVMBuildFCmp(p->builder, LLVMRealOLT, v, min, ""), min, v, "");
				res.value = LLVMBuildSelect(p->builder, LLVMBuildFCmp(p->builder, LLVMRealOGT, v, max, ""), max, v, "");
			} else if (is_signed) {
				v = LLVMBuildSelect(p->builder, LLVMBuildICmp(p->builder, LLVMIntSLT, v, min, ""), min, v, "");
				res.value = LLVMBuildSelect(p->builder, LLVMBuildICmp(p->builder, LLVMIntSGT, v, max, ""), max, v, "");
			} else {
				v = LLVMBuildSelect(p->builder, LLVMBuildICmp(p->builder, LLVMIntULT, v, min, ""), min, v, "");
				res.value = LLVMBuildSelect(p->builder, LLVMBuildICmp(p->builder, LLVMIntUGT, v, max, ""), max, v, "");
			}
			return res;
		}

	case BuiltinProc_simd_to_bits:
		{
			res.value = LLVMBuildBitCast(p->builder, arg0.value, lb_type(m, tv.type), "");
			return res;
		}


	case BuiltinProc_simd_gather:
	case BuiltinProc_simd_scatter:
	case BuiltinProc_simd_masked_load:
	case BuiltinProc_simd_masked_store:
	case BuiltinProc_simd_masked_expand_load:
	case BuiltinProc_simd_masked_compress_store:
		{
			LLVMValueRef ptr = arg0.value;
			LLVMValueRef val = arg1.value;
			LLVMValueRef mask = arg2.value;

			unsigned count = cast(unsigned)get_array_type_count(arg1.type);

			LLVMTypeRef mask_type = LLVMVectorType(LLVMInt1TypeInContext(p->module->ctx), count);
			mask = LLVMBuildTrunc(p->builder, mask, mask_type, "");

			char const *name = nullptr;
			switch (builtin_id) {
			case BuiltinProc_simd_gather:                name = "llvm.masked.gather";        break;
			case BuiltinProc_simd_scatter:               name = "llvm.masked.scatter";       break;
			case BuiltinProc_simd_masked_load:           name = "llvm.masked.load";          break;
			case BuiltinProc_simd_masked_store:          name = "llvm.masked.store";         break;
			case BuiltinProc_simd_masked_expand_load:    name = "llvm.masked.expandload";    break;
			case BuiltinProc_simd_masked_compress_store: name = "llvm.masked.compressstore"; break;
			}
			unsigned type_count = 2;
			LLVMTypeRef types[2] = {
				lb_type(p->module, arg1.type),
				lb_type(p->module, arg0.type)
			};

			auto alignment = cast(unsigned long long)type_align_of(base_array_type(arg1.type));
			LLVMValueRef align = LLVMConstInt(LLVMInt32TypeInContext(p->module->ctx), alignment, false);

			unsigned arg_count = 4;
			LLVMValueRef args[4] = {};
			switch (builtin_id) {
			case BuiltinProc_simd_masked_load:
				types[1] = lb_type(p->module, t_rawptr);
				/*fallthrough*/
			case BuiltinProc_simd_gather:
				args[0] = ptr;
				args[1] = align;
				args[2] = mask;
				args[3] = val;
				break;

			case BuiltinProc_simd_masked_store:
				types[1] = lb_type(p->module, t_rawptr);
				/*fallthrough*/
			case BuiltinProc_simd_scatter:
				args[0] = val;
				args[1] = ptr;
				args[2] = align;
				args[3] = mask;
				break;

			case BuiltinProc_simd_masked_expand_load:
				arg_count = 3;
				type_count = 1;
				args[0] = ptr;
				args[1] = mask;
				args[2] = val;
				break;

			case BuiltinProc_simd_masked_compress_store:
				arg_count = 3;
				type_count = 1;
				args[0] = val;
				args[1] = ptr;
				args[2] = mask;
				break;
			}

			res.value = lb_call_intrinsic(p, name, args, arg_count, types, type_count);
			return res;

		}
	}
	GB_PANIC("Unhandled simd intrinsic: '%.*s'", LIT(builtin_procs[builtin_id].name));

	return {};
}


gb_internal lbValue lb_build_builtin_proc(lbProcedure *p, Ast *expr, TypeAndValue const &tv, BuiltinProcId id) {
	ast_node(ce, CallExpr, expr);

	if (BuiltinProc__simd_begin < id && id < BuiltinProc__simd_end) {
		return lb_build_builtin_simd_proc(p, expr, tv, id);
	}

	switch (id) {
	case BuiltinProc_DIRECTIVE: {
		ast_node(bd, BasicDirective, ce->proc);
		String name = bd->name.string;
		if (name == "location") {
			String procedure = p->entity->token.string;
			TokenPos pos = ast_token(ce->proc).pos;
			if (ce->args.count > 0) {
				Ast *ident = unselector_expr(ce->args[0]);
				GB_ASSERT(ident->kind == Ast_Ident);
				Entity *e = entity_of_node(ident);
				GB_ASSERT(e != nullptr);

				if (e->parent_proc_decl != nullptr && e->parent_proc_decl->entity != nullptr) {
					procedure = e->parent_proc_decl->entity->token.string;
				} else {
					procedure = str_lit("");
				}
				pos = e->token.pos;

			}
			return lb_emit_source_code_location_as_global(p, procedure, pos);
		} else if (name == "load_directory") {
			lbModule *m = p->module;
			TEMPORARY_ALLOCATOR_GUARD();
			LoadDirectoryCache *cache = map_must_get(&m->info->load_directory_map, expr);
			isize count = cache->files.count;

			LLVMValueRef *elements = gb_alloc_array(temporary_allocator(), LLVMValueRef, count);
			for_array(i, cache->files) {
				LoadFileCache *file = cache->files[i];

				String file_name = filename_without_directory(file->path);

				LLVMValueRef values[2] = {};
				values[0] = lb_const_string(m, file_name).value;
				values[1] = lb_const_value(m, t_u8_slice, exact_value_string(file->data)).value;
				LLVMValueRef element = llvm_const_named_struct(m, t_load_directory_file, values, gb_count_of(values));
				elements[i] = element;
			}

			LLVMValueRef backing_array = llvm_const_array(lb_type(m, t_load_directory_file), elements, count);

			Type *array_type = alloc_type_array(t_load_directory_file, count);
			lbAddr backing_array_addr = lb_add_global_generated_from_procedure(p, array_type, {backing_array, array_type});
			lb_make_global_private_const(backing_array_addr);

			LLVMValueRef backing_array_ptr = backing_array_addr.addr.value;
			backing_array_ptr = LLVMConstPointerCast(backing_array_ptr, lb_type(m, t_load_directory_file_ptr));

			LLVMValueRef const_slice = llvm_const_slice_internal(m, backing_array_ptr, LLVMConstInt(lb_type(m, t_int), count, false));

			lbAddr addr = lb_add_global_generated_from_procedure(p, tv.type, {const_slice, t_load_directory_file_slice});
			lb_make_global_private_const(addr);

			return lb_addr_load(p, addr);
		} else {
			GB_PANIC("UNKNOWN DIRECTIVE: %.*s", LIT(name));
		}
	}

	case BuiltinProc_type_info_of: {
		Ast *arg = ce->args[0];
		TypeAndValue tav = type_and_value_of_expr(arg);
		if (tav.mode == Addressing_Type) {
			Type *t = default_type(type_of_expr(arg));
			return lb_type_info(p, t);
		}
		GB_ASSERT(is_type_typeid(tav.type));

		auto args = array_make<lbValue>(permanent_allocator(), 1);
		args[0] = lb_build_expr(p, arg);
		return lb_emit_runtime_call(p, "__type_info_of", args);
	}

	case BuiltinProc_typeid_of: {
		Ast *arg = ce->args[0];
		TypeAndValue tav = type_and_value_of_expr(arg);
		GB_ASSERT(tav.mode == Addressing_Type);
		Type *t = default_type(type_of_expr(arg));
		return lb_typeid(p->module, t);
	}

	case BuiltinProc_len: {
		lbValue v = lb_build_expr(p, ce->args[0]);
		Type *t = base_type(v.type);
		if (is_type_pointer(t)) {
			v = lb_emit_load(p, v);
			t = type_deref(t);
		}
		if (is_type_cstring(t)) {
			return lb_cstring_len(p, v);
		} else if (is_type_string(t)) {
			return lb_string_len(p, v);
		} else if (is_type_array(t)) {
			GB_PANIC("Array lengths are constant");
		} else if (is_type_slice(t)) {
			return lb_slice_len(p, v);
		} else if (is_type_dynamic_array(t)) {
			return lb_dynamic_array_len(p, v);
		} else if (is_type_map(t)) {
			return lb_map_len(p, v);
		} else if (is_type_soa_struct(t)) {
			return lb_soa_struct_len(p, v);
		}

		GB_PANIC("Unreachable");
		break;
	}

	case BuiltinProc_cap: {
		lbValue v = lb_build_expr(p, ce->args[0]);
		Type *t = base_type(v.type);
		if (is_type_pointer(t)) {
			v = lb_emit_load(p, v);
			t = type_deref(t);
		}
		if (is_type_string(t)) {
			GB_PANIC("Unreachable");
		} else if (is_type_array(t)) {
			GB_PANIC("Array lengths are constant");
		} else if (is_type_slice(t)) {
			return lb_slice_len(p, v);
		} else if (is_type_dynamic_array(t)) {
			return lb_dynamic_array_cap(p, v);
		} else if (is_type_map(t)) {
			return lb_map_cap(p, v);
		} else if (is_type_soa_struct(t)) {
			return lb_soa_struct_cap(p, v);
		}

		GB_PANIC("Unreachable");

		break;
	}

	case BuiltinProc_swizzle: {
		isize index_count = ce->args.count-1;
		if (is_type_simd_vector(tv.type)) {
			lbValue vec = lb_build_expr(p, ce->args[0]);
			if (index_count == 0) {
				return vec;
			}

			unsigned mask_len = cast(unsigned)index_count;
			LLVMValueRef *mask_elems = gb_alloc_array(permanent_allocator(), LLVMValueRef, index_count);
			for (isize i = 1; i < ce->args.count; i++) {
				TypeAndValue tv = type_and_value_of_expr(ce->args[i]);
				GB_ASSERT(is_type_integer(tv.type));
				GB_ASSERT(tv.value.kind == ExactValue_Integer);

				u32 index = cast(u32)big_int_to_i64(&tv.value.value_integer);
				mask_elems[i-1] = LLVMConstInt(lb_type(p->module, t_u32), index, false);
			}

			LLVMValueRef mask = LLVMConstVector(mask_elems, mask_len);

			LLVMValueRef v1 = vec.value;
			LLVMValueRef v2 = vec.value;

			lbValue res = {};
			res.type = tv.type;
			res.value = LLVMBuildShuffleVector(p->builder, v1, v2, mask, "");
			return res;
		}

		lbAddr addr = lb_build_array_swizzle_addr(p, ce, tv);
		return lb_addr_load(p, addr);
	}

	case BuiltinProc_complex: {
		lbValue real = lb_build_expr(p, ce->args[0]);
		lbValue imag = lb_build_expr(p, ce->args[1]);
		lbAddr dst_addr = lb_add_local_generated(p, tv.type, false);
		lbValue dst = lb_addr_get_ptr(p, dst_addr);

		Type *ft = base_complex_elem_type(tv.type);
		real = lb_emit_conv(p, real, ft);
		imag = lb_emit_conv(p, imag, ft);
		lb_emit_store(p, lb_emit_struct_ep(p, dst, 0), real);
		lb_emit_store(p, lb_emit_struct_ep(p, dst, 1), imag);

		return lb_emit_load(p, dst);
	}

	case BuiltinProc_quaternion: {
		lbValue xyzw[4] = {};
		for (i32 i = 0; i < 4; i++) {
			ast_node(f, FieldValue, ce->args[i]);
			GB_ASSERT(f->field->kind == Ast_Ident);
			String name = f->field->Ident.token.string;
			i32 index = -1;

			// @QuaternionLayout
			if (name == "x" || name == "imag") {
				index = 0;
			} else if (name == "y" || name == "jmag") {
				index = 1;
			} else if (name == "z" || name == "kmag") {
				index = 2;
			} else if (name == "w" || name == "real") {
				index = 3;
			}
			GB_ASSERT(index >= 0);

			xyzw[index] = lb_build_expr(p, f->value);
		}


		lbAddr dst_addr = lb_add_local_generated(p, tv.type, false);
		lbValue dst = lb_addr_get_ptr(p, dst_addr);

		Type *ft = base_complex_elem_type(tv.type);
		xyzw[0] = lb_emit_conv(p, xyzw[0], ft);
		xyzw[1] = lb_emit_conv(p, xyzw[1], ft);
		xyzw[2] = lb_emit_conv(p, xyzw[2], ft);
		xyzw[3] = lb_emit_conv(p, xyzw[3], ft);
		lb_emit_store(p, lb_emit_struct_ep(p, dst, 0), xyzw[0]);
		lb_emit_store(p, lb_emit_struct_ep(p, dst, 1), xyzw[1]);
		lb_emit_store(p, lb_emit_struct_ep(p, dst, 2), xyzw[2]);
		lb_emit_store(p, lb_emit_struct_ep(p, dst, 3), xyzw[3]);

		return lb_emit_load(p, dst);
	}

	case BuiltinProc_real: {
		lbValue val = lb_build_expr(p, ce->args[0]);
		if (is_type_complex(val.type)) {
			lbValue real = lb_emit_struct_ev(p, val, 0);
			return lb_emit_conv(p, real, tv.type);
		} else if (is_type_quaternion(val.type)) {
			// @QuaternionLayout
			lbValue real = lb_emit_struct_ev(p, val, 3);
			return lb_emit_conv(p, real, tv.type);
		}
		GB_PANIC("invalid type for real");
		return {};
	}
	case BuiltinProc_imag: {
		lbValue val = lb_build_expr(p, ce->args[0]);
		if (is_type_complex(val.type)) {
			lbValue imag = lb_emit_struct_ev(p, val, 1);
			return lb_emit_conv(p, imag, tv.type);
		} else if (is_type_quaternion(val.type)) {
			// @QuaternionLayout
			lbValue imag = lb_emit_struct_ev(p, val, 0);
			return lb_emit_conv(p, imag, tv.type);
		}
		GB_PANIC("invalid type for imag");
		return {};
	}
	case BuiltinProc_jmag: {
		lbValue val = lb_build_expr(p, ce->args[0]);
		if (is_type_quaternion(val.type)) {
			// @QuaternionLayout
			lbValue imag = lb_emit_struct_ev(p, val, 1);
			return lb_emit_conv(p, imag, tv.type);
		}
		GB_PANIC("invalid type for jmag");
		return {};
	}
	case BuiltinProc_kmag: {
		lbValue val = lb_build_expr(p, ce->args[0]);
		if (is_type_quaternion(val.type)) {
			// @QuaternionLayout
			lbValue imag = lb_emit_struct_ev(p, val, 2);
			return lb_emit_conv(p, imag, tv.type);
		}
		GB_PANIC("invalid type for kmag");
		return {};
	}

	case BuiltinProc_conj: {
		lbValue val = lb_build_expr(p, ce->args[0]);
		return lb_emit_conjugate(p, val, tv.type);
	}

	case BuiltinProc_expand_values: {
		lbValue val = lb_build_expr(p, ce->args[0]);
		Type *t = base_type(val.type);

		if (!is_type_tuple(tv.type)) {
			if (t->kind == Type_Struct) {
				GB_ASSERT(t->Struct.fields.count == 1);
				return lb_emit_struct_ev(p, val, 0);
			} else if (t->kind == Type_Array) {
				GB_ASSERT(t->Array.count == 1);
				return lb_emit_struct_ev(p, val, 0);
			} else {
				GB_PANIC("Unknown type of expand_values");
			}

		}

		GB_ASSERT(is_type_tuple(tv.type));
		// NOTE(bill): Doesn't need to be zero because it will be initialized in the loops
		lbValue tuple = lb_addr_get_ptr(p, lb_add_local_generated(p, tv.type, false));
		if (t->kind == Type_Struct) {
			for_array(src_index, t->Struct.fields) {
				Entity *field = t->Struct.fields[src_index];
				i32 field_index = field->Variable.field_index;
				lbValue f = lb_emit_struct_ev(p, val, field_index);
				lbValue ep = lb_emit_struct_ep(p, tuple, cast(i32)src_index);
				lb_emit_store(p, ep, f);
			}
		} else if (is_type_array_like(t)) {
			// TODO(bill): Clean-up this code
			lbValue ap = lb_address_from_load_or_generate_local(p, val);
			i32 n = cast(i32)get_array_type_count(t);
			for (i32 i = 0; i < n; i++) {
				lbValue f = lb_emit_load(p, lb_emit_array_epi(p, ap, i));
				lbValue ep = lb_emit_struct_ep(p, tuple, i);
				lb_emit_store(p, ep, f);
			}
		} else {
			GB_PANIC("Unknown type of expand_values");
		}
		return lb_emit_load(p, tuple);
	}

	case BuiltinProc_compress_values: {
		isize value_count = 0;
		for (Ast *arg : ce->args) {
			Type *t = arg->tav.type;
			if (is_type_tuple(t)) {
				value_count += t->Tuple.variables.count;
			} else {
				value_count += 1;
			}
		}

		if (value_count == 1) {
			lbValue x = lb_build_expr(p, ce->args[0]);
			x = lb_emit_conv(p, x, tv.type);
			return x;
		}

		Type *dt = base_type(tv.type);
		lbAddr addr = lb_add_local_generated(p, tv.type, true);
		if (is_type_struct(dt) || is_type_tuple(dt)) {
			i32 index = 0;
			for (Ast *arg : ce->args) {
				lbValue x = lb_build_expr(p, arg);
				if (is_type_tuple(x.type)) {
					for (isize i = 0; i < x.type->Tuple.variables.count; i++) {
						lbValue y = lb_emit_tuple_ev(p, x, cast(i32)i);
						lbValue ptr = lb_emit_struct_ep(p, addr.addr, index++);
						y = lb_emit_conv(p, y, type_deref(ptr.type));
						lb_emit_store(p, ptr, y);
					}
				} else {
					lbValue ptr = lb_emit_struct_ep(p, addr.addr, index++);
					x = lb_emit_conv(p, x, type_deref(ptr.type));
					lb_emit_store(p, ptr, x);
				}
			}
			GB_ASSERT(index == value_count);
		} else if (is_type_array_like(dt)) {
			i32 index = 0;
			for (Ast *arg : ce->args) {
				lbValue x = lb_build_expr(p, arg);
				if (is_type_tuple(x.type)) {
					for (isize i = 0; i < x.type->Tuple.variables.count; i++) {
						lbValue y = lb_emit_tuple_ev(p, x, cast(i32)i);
						lbValue ptr = lb_emit_array_epi(p, addr.addr, index++);
						y = lb_emit_conv(p, y, type_deref(ptr.type));
						lb_emit_store(p, ptr, y);
					}
				} else {
					lbValue ptr = lb_emit_array_epi(p, addr.addr, index++);
					x = lb_emit_conv(p, x, type_deref(ptr.type));
					lb_emit_store(p, ptr, x);
				}
			}
			GB_ASSERT(index == value_count);
		} else {
			GB_PANIC("TODO(bill): compress_values -> %s", type_to_string(tv.type));
		}

		return lb_addr_load(p, addr);
	}

	case BuiltinProc_min: {
		Type *t = type_of_expr(expr);
		if (ce->args.count == 2) {
			return lb_emit_min(p, t, lb_build_expr(p, ce->args[0]), lb_build_expr(p, ce->args[1]));
		} else {
			lbValue x = lb_build_expr(p, ce->args[0]);
			for (isize i = 1; i < ce->args.count; i++) {
				x = lb_emit_min(p, t, x, lb_build_expr(p, ce->args[i]));
			}
			return x;
		}
	}

	case BuiltinProc_max: {
		Type *t = type_of_expr(expr);
		if (ce->args.count == 2) {
			return lb_emit_max(p, t, lb_build_expr(p, ce->args[0]), lb_build_expr(p, ce->args[1]));
		} else {
			lbValue x = lb_build_expr(p, ce->args[0]);
			for (isize i = 1; i < ce->args.count; i++) {
				x = lb_emit_max(p, t, x, lb_build_expr(p, ce->args[i]));
			}
			return x;
		}
	}

	case BuiltinProc_abs: {
		lbValue x = lb_build_expr(p, ce->args[0]);
		Type *t = x.type;
		if (is_type_unsigned(t)) {
			return x;
		}
		if (is_type_quaternion(t)) {
			i64 sz = 8*type_size_of(t);
			auto args = array_make<lbValue>(permanent_allocator(), 1);
			args[0] = x;
			switch (sz) {
			case 64:  return lb_emit_runtime_call(p, "abs_quaternion64", args);
			case 128: return lb_emit_runtime_call(p, "abs_quaternion128", args);
			case 256: return lb_emit_runtime_call(p, "abs_quaternion256", args);
			}
			GB_PANIC("Unknown complex type");
		} else if (is_type_complex(t)) {
			i64 sz = 8*type_size_of(t);
			auto args = array_make<lbValue>(permanent_allocator(), 1);
			args[0] = x;
			switch (sz) {
			case 32:  return lb_emit_runtime_call(p, "abs_complex32",  args);
			case 64:  return lb_emit_runtime_call(p, "abs_complex64",  args);
			case 128: return lb_emit_runtime_call(p, "abs_complex128", args);
			}
			GB_PANIC("Unknown complex type");
		} else if (is_type_float(t)) {
			bool little = is_type_endian_little(t) || (is_type_endian_platform(t) && build_context.endian_kind == TargetEndian_Little);
			Type *t_unsigned = nullptr;
			lbValue mask = {0};
			switch (type_size_of(t)) {
			case 2:
				t_unsigned = t_u16;
				mask = lb_const_int(p->module, t_unsigned, little ? 0x7FFF : 0xFF7F);
				break;
			case 4:
				t_unsigned = t_u32;
				mask = lb_const_int(p->module, t_unsigned, little ? 0x7FFFFFFF : 0xFFFFFF7F);
				break;
			case 8:
				t_unsigned = t_u64;
				mask = lb_const_int(p->module, t_unsigned, little ? 0x7FFFFFFFFFFFFFFF : 0xFFFFFFFFFFFFFF7F);
				break;
			default:
				GB_PANIC("abs: unhandled float size");
			}

			lbValue as_unsigned = lb_emit_transmute(p, x, t_unsigned);
			lbValue abs = lb_emit_arith(p, Token_And, as_unsigned, mask, t_unsigned);
			return lb_emit_transmute(p, abs, t);
		}

		lbValue zero = lb_const_nil(p->module, t);
		lbValue cond = lb_emit_comp(p, Token_Lt, x, zero);
		lbValue neg = lb_emit_unary_arith(p, Token_Sub, x, t);
		return lb_emit_select(p, cond, neg, x);
	}

	case BuiltinProc_clamp:
		return lb_emit_clamp(p, type_of_expr(expr),
		                     lb_build_expr(p, ce->args[0]),
		                     lb_build_expr(p, ce->args[1]),
		                     lb_build_expr(p, ce->args[2]));


	case BuiltinProc_soa_zip:
		return lb_soa_zip(p, ce, tv);
	case BuiltinProc_soa_unzip:
		return lb_soa_unzip(p, ce, tv);

	case BuiltinProc_transpose:
		{
			lbValue m = lb_build_expr(p, ce->args[0]);
			return lb_emit_matrix_tranpose(p, m, tv.type);
		}

	case BuiltinProc_outer_product:
		{
			lbValue a = lb_build_expr(p, ce->args[0]);
			lbValue b = lb_build_expr(p, ce->args[1]);
			return lb_emit_outer_product(p, a, b, tv.type);
		}
	case BuiltinProc_hadamard_product:
		{
			lbValue a = lb_build_expr(p, ce->args[0]);
			lbValue b = lb_build_expr(p, ce->args[1]);
			if (is_type_array(tv.type)) {
				return lb_emit_arith(p, Token_Mul, a, b, tv.type);
			}
			GB_ASSERT(is_type_matrix(tv.type));
			return lb_emit_arith_matrix(p, Token_Mul, a, b, tv.type, true);
		}

	case BuiltinProc_matrix_flatten:
		{
			lbValue m = lb_build_expr(p, ce->args[0]);
			return lb_emit_matrix_flatten(p, m, tv.type);
		}

	case BuiltinProc_unreachable:
		lb_emit_unreachable(p);
		return {};

	case BuiltinProc_raw_data:
		{
			lbValue x = lb_build_expr(p, ce->args[0]);
			Type *t = base_type(x.type);
			lbValue res = {};
			switch (t->kind) {
			case Type_Slice:
				res = lb_slice_elem(p, x);
				res = lb_emit_conv(p, res, tv.type);
				break;
			case Type_DynamicArray:
				res = lb_dynamic_array_elem(p, x);
				res = lb_emit_conv(p, res, tv.type);
				break;
			case Type_Basic:
				if (t->Basic.kind == Basic_string) {
					res = lb_string_elem(p, x);
					res = lb_emit_conv(p, res, tv.type);
				} else if (t->Basic.kind == Basic_cstring) {
					res = lb_emit_conv(p, x, tv.type);
				}
				break;
			case Type_Pointer:
			case Type_MultiPointer:
				res = lb_emit_conv(p, x, tv.type);
				break;
			}
			GB_ASSERT(res.value != nullptr);
			return res;
		}


	// "Intrinsics"

	case BuiltinProc_alloca:
		{
			lbValue sz = lb_build_expr(p, ce->args[0]);
			i64 al = exact_value_to_i64(type_and_value_of_expr(ce->args[1]).value);

			lbValue res = {};
			res.type = alloc_type_multi_pointer(t_u8);
			res.value = LLVMBuildArrayAlloca(p->builder, lb_type(p->module, t_u8), sz.value, "");
			LLVMSetAlignment(res.value, cast(unsigned)al);
			return res;
		}

	case BuiltinProc_cpu_relax:
		if (build_context.metrics.arch == TargetArch_i386 ||
			build_context.metrics.arch == TargetArch_amd64) {
			LLVMTypeRef func_type = LLVMFunctionType(LLVMVoidTypeInContext(p->module->ctx), nullptr, 0, false);
			LLVMValueRef the_asm = llvm_get_inline_asm(func_type, str_lit("pause"), {}, true);
			GB_ASSERT(the_asm != nullptr);
			LLVMBuildCall2(p->builder, func_type, the_asm, nullptr, 0, "");
		} else if (build_context.metrics.arch == TargetArch_arm64) {
			LLVMTypeRef func_type = LLVMFunctionType(LLVMVoidTypeInContext(p->module->ctx), nullptr, 0, false);
			// NOTE(bill, 2022-03-30): `isb` appears to a better option that `yield`
			// See: https://bugs.java.com/bugdatabase/view_bug.do?bug_id=8258604
			LLVMValueRef the_asm = llvm_get_inline_asm(func_type, str_lit("isb"), {}, true);
			GB_ASSERT(the_asm != nullptr);
			LLVMBuildCall2(p->builder, func_type, the_asm, nullptr, 0, "");
		} else {
			// NOTE: default to something to prevent optimization
			LLVMTypeRef func_type = LLVMFunctionType(LLVMVoidTypeInContext(p->module->ctx), nullptr, 0, false);
			LLVMValueRef the_asm = llvm_get_inline_asm(func_type, str_lit(""), {}, true);
			GB_ASSERT(the_asm != nullptr);
			LLVMBuildCall2(p->builder, func_type, the_asm, nullptr, 0, "");
		}
		return {};


	case BuiltinProc_debug_trap:
	case BuiltinProc_trap:
		{
			char const *name = nullptr;
			switch (id) {
			case BuiltinProc_debug_trap: name = "llvm.debugtrap"; break;
			case BuiltinProc_trap:       name = "llvm.trap";      break;
			}

			lb_call_intrinsic(p, name, nullptr, 0, nullptr, 0);
			if (id == BuiltinProc_trap) {
				LLVMBuildUnreachable(p->builder);
			}
			return {};
		}

	case BuiltinProc_read_cycle_counter:
		{
			lbValue res = {};
			res.type = tv.type;

			if (build_context.metrics.arch == TargetArch_arm64) {
				LLVMTypeRef func_type = LLVMFunctionType(LLVMInt64TypeInContext(p->module->ctx), nullptr, 0, false);
				bool has_side_effects = false;
				LLVMValueRef the_asm = llvm_get_inline_asm(func_type, str_lit("mrs $0, cntvct_el0"), str_lit("=r"), has_side_effects);
				GB_ASSERT(the_asm != nullptr);
				res.value = LLVMBuildCall2(p->builder, func_type, the_asm, nullptr, 0, "");
			} else {
				char const *name = "llvm.readcyclecounter";
				res.value = lb_call_intrinsic(p, name, nullptr, 0, nullptr, 0);
			}
			return res;
		}
	case BuiltinProc_read_cycle_counter_frequency:
		{
			lbValue res = {};
			res.type = tv.type;

			if (build_context.metrics.arch == TargetArch_arm64) {
				LLVMTypeRef func_type = LLVMFunctionType(LLVMInt64TypeInContext(p->module->ctx), nullptr, 0, false);
				bool has_side_effects = false;
				LLVMValueRef the_asm = llvm_get_inline_asm(func_type, str_lit("mrs $0, cntfrq_el0"), str_lit("=r"), has_side_effects);
				GB_ASSERT(the_asm != nullptr);
				res.value = LLVMBuildCall2(p->builder, func_type, the_asm, nullptr, 0, "");
			}

			return res;
		}

	case BuiltinProc_count_trailing_zeros:
		return lb_emit_count_trailing_zeros(p, lb_build_expr(p, ce->args[0]), tv.type);
	case BuiltinProc_count_leading_zeros:
		return lb_emit_count_leading_zeros(p, lb_build_expr(p, ce->args[0]), tv.type);

	case BuiltinProc_count_ones:
		return lb_emit_count_ones(p, lb_build_expr(p, ce->args[0]), tv.type);
	case BuiltinProc_count_zeros:
		return lb_emit_count_zeros(p, lb_build_expr(p, ce->args[0]), tv.type);

	case BuiltinProc_reverse_bits:
		return lb_emit_reverse_bits(p, lb_build_expr(p, ce->args[0]), tv.type);

	case BuiltinProc_byte_swap:
		{
			lbValue x = lb_build_expr(p, ce->args[0]);
			x = lb_emit_conv(p, x, tv.type);
			return lb_emit_byte_swap(p, x, tv.type);
		}

	case BuiltinProc_overflow_add:
	case BuiltinProc_overflow_sub:
	case BuiltinProc_overflow_mul:
		{
			Type *main_type = tv.type;
			Type *type = main_type;
			if (is_type_tuple(main_type)) {
				type = main_type->Tuple.variables[0]->type;
			}

			lbValue x = lb_build_expr(p, ce->args[0]);
			lbValue y = lb_build_expr(p, ce->args[1]);
			x = lb_emit_conv(p, x, type);
			y = lb_emit_conv(p, y, type);

			char const *name = nullptr;
			if (is_type_unsigned(type)) {
				switch (id) {
				case BuiltinProc_overflow_add: name = "llvm.uadd.with.overflow"; break;
				case BuiltinProc_overflow_sub: name = "llvm.usub.with.overflow"; break;
				case BuiltinProc_overflow_mul: name = "llvm.umul.with.overflow"; break;
				}
			} else {
				switch (id) {
				case BuiltinProc_overflow_add: name = "llvm.sadd.with.overflow"; break;
				case BuiltinProc_overflow_sub: name = "llvm.ssub.with.overflow"; break;
				case BuiltinProc_overflow_mul: name = "llvm.smul.with.overflow"; break;
				}
			}
			LLVMTypeRef types[1] = {lb_type(p->module, type)};

			LLVMValueRef args[2] = { x.value, y.value };

			lbValue res = {};
			res.value = lb_call_intrinsic(p, name, args, gb_count_of(args), types, gb_count_of(types));

			if (is_type_tuple(main_type)) {
				Type *res_type = nullptr;
				gbAllocator a = permanent_allocator();
				res_type = alloc_type_tuple();
				slice_init(&res_type->Tuple.variables, a, 2);
				res_type->Tuple.variables[0] = alloc_entity_field(nullptr, blank_token, type,        false, 0);
				res_type->Tuple.variables[1] = alloc_entity_field(nullptr, blank_token, t_llvm_bool, false, 1);

				res.type = res_type;
			} else {
				res.value = LLVMBuildExtractValue(p->builder, res.value, 0, "");
				res.type = type;
			}
			return res;
		}

	case BuiltinProc_saturating_add:
	case BuiltinProc_saturating_sub:
		{
			Type *main_type = tv.type;
			Type *type = main_type;

			lbValue x = lb_build_expr(p, ce->args[0]);
			lbValue y = lb_build_expr(p, ce->args[1]);
			x = lb_emit_conv(p, x, type);
			y = lb_emit_conv(p, y, type);

			char const *name = nullptr;
			if (is_type_unsigned(type)) {
				switch (id) {
				case BuiltinProc_saturating_add: name = "llvm.uadd.sat"; break;
				case BuiltinProc_saturating_sub: name = "llvm.usub.sat"; break;
				}
			} else {
				switch (id) {
				case BuiltinProc_saturating_add: name = "llvm.sadd.sat"; break;
				case BuiltinProc_saturating_sub: name = "llvm.ssub.sat"; break;
				}
			}
			LLVMTypeRef types[1] = {lb_type(p->module, type)};

			LLVMValueRef args[2] = { x.value, y.value };

			lbValue res = {};
			res.value = lb_call_intrinsic(p, name, args, gb_count_of(args), types, gb_count_of(types));
			res.type = type;
			return res;
		}

	case BuiltinProc_sqrt:
		{
			Type *type = tv.type;

			lbValue x = lb_build_expr(p, ce->args[0]);
			x = lb_emit_conv(p, x, type);

			char const *name = "llvm.sqrt";
			LLVMTypeRef types[1] = {lb_type(p->module, type)};

			LLVMValueRef args[1] = { x.value };

			lbValue res = {};
			res.value = lb_call_intrinsic(p, name, args, gb_count_of(args), types, gb_count_of(types));
			res.type = type;
			return res;
		}

	case BuiltinProc_fused_mul_add:
		{
			Type *type = tv.type;
			lbValue x = lb_emit_conv(p, lb_build_expr(p, ce->args[0]), type);
			lbValue y = lb_emit_conv(p, lb_build_expr(p, ce->args[1]), type);
			lbValue z = lb_emit_conv(p, lb_build_expr(p, ce->args[2]), type);


			char const *name = "llvm.fma";
			LLVMTypeRef types[1] = {lb_type(p->module, type)};

			LLVMValueRef args[3] = { x.value, y.value, z.value };

			lbValue res = {};
			res.value = lb_call_intrinsic(p, name, args, gb_count_of(args), types, gb_count_of(types));
			res.type = type;
			return res;
		}

	case BuiltinProc_mem_copy:
		{
			lbValue dst = lb_build_expr(p, ce->args[0]);
			lbValue src = lb_build_expr(p, ce->args[1]);
			lbValue len = lb_build_expr(p, ce->args[2]);

			lb_mem_copy_overlapping(p, dst, src, len, false);
			return {};
		}
	case BuiltinProc_mem_copy_non_overlapping:
		{
			lbValue dst = lb_build_expr(p, ce->args[0]);
			lbValue src = lb_build_expr(p, ce->args[1]);
			lbValue len = lb_build_expr(p, ce->args[2]);

			lb_mem_copy_non_overlapping(p, dst, src, len, false);
			return {};
		}

	case BuiltinProc_mem_zero:
		{
			lbValue ptr = lb_build_expr(p, ce->args[0]);
			lbValue len = lb_build_expr(p, ce->args[1]);
			ptr = lb_emit_conv(p, ptr, t_rawptr);
			len = lb_emit_conv(p, len, t_int);

			unsigned alignment = 1;
			lb_mem_zero_ptr_internal(p, ptr.value, len.value, alignment, false);
			return {};
		}
	case BuiltinProc_mem_zero_volatile:
		{
			lbValue ptr = lb_build_expr(p, ce->args[0]);
			lbValue len = lb_build_expr(p, ce->args[1]);
			ptr = lb_emit_conv(p, ptr, t_rawptr);
			len = lb_emit_conv(p, len, t_int);

			unsigned alignment = 1;
			lb_mem_zero_ptr_internal(p, ptr.value, len.value, alignment, true);
			return {};
		}

	case BuiltinProc_ptr_offset:
		{
			lbValue ptr = lb_build_expr(p, ce->args[0]);
			lbValue len = lb_build_expr(p, ce->args[1]);
			len = lb_emit_conv(p, len, t_int);
			return lb_emit_ptr_offset(p, ptr, len);
		}
	case BuiltinProc_ptr_sub:
		{
			Type *elem0 = type_deref(type_of_expr(ce->args[0]), true);
			Type *elem1 = type_deref(type_of_expr(ce->args[1]), true);
			GB_ASSERT(are_types_identical(elem0, elem1));
			Type *elem = elem0;

			lbValue ptr0 = lb_emit_conv(p, lb_build_expr(p, ce->args[0]), t_uintptr);
			lbValue ptr1 = lb_emit_conv(p, lb_build_expr(p, ce->args[1]), t_uintptr);
			ptr0 = lb_emit_conv(p, ptr0, t_int);
			ptr1 = lb_emit_conv(p, ptr1, t_int);

			lbValue diff = lb_emit_arith(p, Token_Sub, ptr0, ptr1, t_int);
			return lb_emit_arith(p, Token_Quo, diff, lb_const_int(p->module, t_int, type_size_of(elem)), t_int);
		}


	// TODO(bill): Which is correct?
	case BuiltinProc_atomic_thread_fence:
		LLVMBuildFence(p->builder, llvm_atomic_ordering_from_odin(ce->args[0]), false, "");
		return {};
	case BuiltinProc_atomic_signal_fence:
		LLVMBuildFence(p->builder, llvm_atomic_ordering_from_odin(ce->args[0]), true, "");
		return {};

	case BuiltinProc_volatile_store:
	case BuiltinProc_non_temporal_store:
	case BuiltinProc_atomic_store:
	case BuiltinProc_atomic_store_explicit: {
		lbValue dst = lb_build_expr(p, ce->args[0]);
		lbValue val = lb_build_expr(p, ce->args[1]);
		val = lb_emit_conv(p, val, type_deref(dst.type));

		LLVMValueRef instr = LLVMBuildStore(p->builder, val.value, dst.value);
		switch (id) {
		case BuiltinProc_non_temporal_store:
			{
				unsigned kind_id = LLVMGetMDKindIDInContext(p->module->ctx, "nontemporal", 11);
				LLVMMetadataRef node = LLVMValueAsMetadata(LLVMConstInt(lb_type(p->module, t_u32), 1, false));
				LLVMSetMetadata(instr, kind_id, LLVMMetadataAsValue(p->module->ctx, node));
			}
			break;
		case BuiltinProc_volatile_store:
			LLVMSetVolatile(instr, true);
			break;
		case BuiltinProc_atomic_store:
			LLVMSetOrdering(instr, LLVMAtomicOrderingSequentiallyConsistent);
			LLVMSetVolatile(instr, true);
			break;
		case BuiltinProc_atomic_store_explicit:
			{
				auto ordering = llvm_atomic_ordering_from_odin(ce->args[2]);
				LLVMSetOrdering(instr, ordering);
				LLVMSetVolatile(instr, true);
			}
			break;
		}

		LLVMSetAlignment(instr, cast(unsigned)type_align_of(type_deref(dst.type)));

		return {};
	}

	case BuiltinProc_volatile_load:
	case BuiltinProc_non_temporal_load:
	case BuiltinProc_atomic_load:
	case BuiltinProc_atomic_load_explicit: {
		lbValue dst = lb_build_expr(p, ce->args[0]);

		LLVMValueRef instr = OdinLLVMBuildLoad(p, lb_type(p->module, type_deref(dst.type)), dst.value);
		switch (id) {
		case BuiltinProc_non_temporal_load:
			{
				unsigned kind_id = LLVMGetMDKindIDInContext(p->module->ctx, "nontemporal", 11);
				LLVMMetadataRef node = LLVMValueAsMetadata(LLVMConstInt(lb_type(p->module, t_u32), 1, false));
				LLVMSetMetadata(instr, kind_id, LLVMMetadataAsValue(p->module->ctx, node));
			}
			break;
			break;
		case BuiltinProc_volatile_load:
			LLVMSetVolatile(instr, true);
			break;
		case BuiltinProc_atomic_load:
			LLVMSetOrdering(instr, LLVMAtomicOrderingSequentiallyConsistent);
			LLVMSetVolatile(instr, true);
			break;
		case BuiltinProc_atomic_load_explicit:
			{
				auto ordering = llvm_atomic_ordering_from_odin(ce->args[1]);
				LLVMSetOrdering(instr, ordering);
				LLVMSetVolatile(instr, true);
			}
			break;
		}
		LLVMSetAlignment(instr, cast(unsigned)type_align_of(type_deref(dst.type)));

		lbValue res = {};
		res.value = instr;
		res.type = type_deref(dst.type);
		return res;
	}

	case BuiltinProc_unaligned_store:
		{
			lbValue dst = lb_build_expr(p, ce->args[0]);
			lbValue src = lb_build_expr(p, ce->args[1]);
			Type *t = type_deref(dst.type);

			if (is_type_simd_vector(t)) {
				LLVMValueRef store = LLVMBuildStore(p->builder, src.value, dst.value);
				LLVMSetAlignment(store, 1);
			} else {
				src = lb_address_from_load_or_generate_local(p, src);
				lb_mem_copy_non_overlapping(p, dst, src, lb_const_int(p->module, t_int, type_size_of(t)), false);
			}
			return {};
		}

	case BuiltinProc_unaligned_load:
		{
			lbValue src = lb_build_expr(p, ce->args[0]);
			Type *t = type_deref(src.type);
			if (is_type_simd_vector(t)) {
				lbValue res = {};
				res.type = t;
				res.value = OdinLLVMBuildLoadAligned(p, lb_type(p->module, t), src.value, 1);
				return res;
			} else {
				lbAddr dst = lb_add_local_generated(p, t, false);
				lb_mem_copy_non_overlapping(p, dst.addr, src, lb_const_int(p->module, t_int, type_size_of(t)), false);
				return lb_addr_load(p, dst);
			}
		}

	case BuiltinProc_atomic_add:
	case BuiltinProc_atomic_sub:
	case BuiltinProc_atomic_and:
	case BuiltinProc_atomic_nand:
	case BuiltinProc_atomic_or:
	case BuiltinProc_atomic_xor:
	case BuiltinProc_atomic_exchange:
	case BuiltinProc_atomic_add_explicit:
	case BuiltinProc_atomic_sub_explicit:
	case BuiltinProc_atomic_and_explicit:
	case BuiltinProc_atomic_nand_explicit:
	case BuiltinProc_atomic_or_explicit:
	case BuiltinProc_atomic_xor_explicit:
	case BuiltinProc_atomic_exchange_explicit: {
		lbValue dst = lb_build_expr(p, ce->args[0]);
		lbValue val = lb_build_expr(p, ce->args[1]);
		val = lb_emit_conv(p, val, type_deref(dst.type));

		LLVMAtomicRMWBinOp op = {};
		LLVMAtomicOrdering ordering = {};

		switch (id) {
		case BuiltinProc_atomic_add:               op = LLVMAtomicRMWBinOpAdd;  ordering = LLVMAtomicOrderingSequentiallyConsistent;    break;
		case BuiltinProc_atomic_sub:               op = LLVMAtomicRMWBinOpSub;  ordering = LLVMAtomicOrderingSequentiallyConsistent;    break;
		case BuiltinProc_atomic_and:               op = LLVMAtomicRMWBinOpAnd;  ordering = LLVMAtomicOrderingSequentiallyConsistent;    break;
		case BuiltinProc_atomic_nand:              op = LLVMAtomicRMWBinOpNand; ordering = LLVMAtomicOrderingSequentiallyConsistent;    break;
		case BuiltinProc_atomic_or:                op = LLVMAtomicRMWBinOpOr;   ordering = LLVMAtomicOrderingSequentiallyConsistent;    break;
		case BuiltinProc_atomic_xor:               op = LLVMAtomicRMWBinOpXor;  ordering = LLVMAtomicOrderingSequentiallyConsistent;    break;
		case BuiltinProc_atomic_exchange:          op = LLVMAtomicRMWBinOpXchg; ordering = LLVMAtomicOrderingSequentiallyConsistent;    break;
		case BuiltinProc_atomic_add_explicit:      op = LLVMAtomicRMWBinOpAdd;  ordering = llvm_atomic_ordering_from_odin(ce->args[2]); break;
		case BuiltinProc_atomic_sub_explicit:      op = LLVMAtomicRMWBinOpSub;  ordering = llvm_atomic_ordering_from_odin(ce->args[2]); break;
		case BuiltinProc_atomic_and_explicit:      op = LLVMAtomicRMWBinOpAnd;  ordering = llvm_atomic_ordering_from_odin(ce->args[2]); break;
		case BuiltinProc_atomic_nand_explicit:     op = LLVMAtomicRMWBinOpNand; ordering = llvm_atomic_ordering_from_odin(ce->args[2]); break;
		case BuiltinProc_atomic_or_explicit:       op = LLVMAtomicRMWBinOpOr;   ordering = llvm_atomic_ordering_from_odin(ce->args[2]); break;
		case BuiltinProc_atomic_xor_explicit:      op = LLVMAtomicRMWBinOpXor;  ordering = llvm_atomic_ordering_from_odin(ce->args[2]); break;
		case BuiltinProc_atomic_exchange_explicit: op = LLVMAtomicRMWBinOpXchg; ordering = llvm_atomic_ordering_from_odin(ce->args[2]); break;
		}

		lbValue res = {};
		res.value = LLVMBuildAtomicRMW(p->builder, op, dst.value, val.value, ordering, false);
		res.type = tv.type;
		LLVMSetVolatile(res.value, true);
		return res;
	}

	case BuiltinProc_atomic_compare_exchange_strong:
	case BuiltinProc_atomic_compare_exchange_weak:
	case BuiltinProc_atomic_compare_exchange_strong_explicit:
	case BuiltinProc_atomic_compare_exchange_weak_explicit: {
		lbValue address = lb_build_expr(p, ce->args[0]);
		Type *elem = type_deref(address.type);
		lbValue old_value = lb_build_expr(p, ce->args[1]);
		lbValue new_value = lb_build_expr(p, ce->args[2]);
		old_value = lb_emit_conv(p, old_value, elem);
		new_value = lb_emit_conv(p, new_value, elem);

		LLVMAtomicOrdering success_ordering = {};
		LLVMAtomicOrdering failure_ordering = {};
		LLVMBool weak = false;

		switch (id) {
		case BuiltinProc_atomic_compare_exchange_strong:          success_ordering = LLVMAtomicOrderingSequentiallyConsistent;    failure_ordering = LLVMAtomicOrderingSequentiallyConsistent;    weak = false; break;
		case BuiltinProc_atomic_compare_exchange_weak:            success_ordering = LLVMAtomicOrderingSequentiallyConsistent;    failure_ordering = LLVMAtomicOrderingSequentiallyConsistent;    weak = true;  break;
		case BuiltinProc_atomic_compare_exchange_strong_explicit: success_ordering = llvm_atomic_ordering_from_odin(ce->args[3]); failure_ordering = llvm_atomic_ordering_from_odin(ce->args[4]); weak = false; break;
		case BuiltinProc_atomic_compare_exchange_weak_explicit:   success_ordering = llvm_atomic_ordering_from_odin(ce->args[3]); failure_ordering = llvm_atomic_ordering_from_odin(ce->args[4]); weak = true;  break;
		}

		LLVMBool single_threaded = false;

		LLVMValueRef value = LLVMBuildAtomicCmpXchg(
			p->builder, address.value,
			old_value.value, new_value.value,
			success_ordering,
			failure_ordering,
			single_threaded
		);
		LLVMSetWeak(value, weak);
		LLVMSetVolatile(value, true);

		if (is_type_tuple(tv.type)) {
			Type *fix_typed = alloc_type_tuple();
			slice_init(&fix_typed->Tuple.variables, permanent_allocator(), 2);
			fix_typed->Tuple.variables[0] = tv.type->Tuple.variables[0];
			fix_typed->Tuple.variables[1] = alloc_entity_field(nullptr, blank_token, t_llvm_bool, false, 1);

			lbValue res = {};
			res.value = value;
			res.type = fix_typed;
			return res;
		} else {
			lbValue res = {};
			res.value = LLVMBuildExtractValue(p->builder, value, 0, "");
			res.type = tv.type;
			return res;
		}
	}


	case BuiltinProc_type_equal_proc:
		return lb_equal_proc_for_type(p->module, ce->args[0]->tav.type);

	case BuiltinProc_type_hasher_proc:
		return lb_hasher_proc_for_type(p->module, ce->args[0]->tav.type);

	case BuiltinProc_type_map_info:
		return lb_gen_map_info_ptr(p->module, ce->args[0]->tav.type);

	case BuiltinProc_type_map_cell_info:
		return lb_gen_map_cell_info_ptr(p->module, ce->args[0]->tav.type);


	case BuiltinProc_fixed_point_mul:
	case BuiltinProc_fixed_point_div:
	case BuiltinProc_fixed_point_mul_sat:
	case BuiltinProc_fixed_point_div_sat:
		{
			Type *platform_type = integer_endian_type_to_platform_type(tv.type);

			lbValue x     = lb_emit_conv(p, lb_build_expr(p, ce->args[0]), platform_type);
			lbValue y     = lb_emit_conv(p, lb_build_expr(p, ce->args[1]), platform_type);
			lbValue scale = lb_emit_conv(p, lb_build_expr(p, ce->args[2]), t_i32);

			char const *name = nullptr;
			if (is_type_unsigned(tv.type)) {
				switch (id) {
				case BuiltinProc_fixed_point_mul:     name = "llvm.umul.fix";     break;
				case BuiltinProc_fixed_point_div:     name = "llvm.udiv.fix";     break;
				case BuiltinProc_fixed_point_mul_sat: name = "llvm.umul.fix.sat"; break;
				case BuiltinProc_fixed_point_div_sat: name = "llvm.udiv.fix.sat"; break;
				}
			} else {
				switch (id) {
				case BuiltinProc_fixed_point_mul:     name = "llvm.smul.fix";     break;
				case BuiltinProc_fixed_point_div:     name = "llvm.sdiv.fix";     break;
				case BuiltinProc_fixed_point_mul_sat: name = "llvm.smul.fix.sat"; break;
				case BuiltinProc_fixed_point_div_sat: name = "llvm.sdiv.fix.sat"; break;
				}
			}
			GB_ASSERT(name != nullptr);

			LLVMTypeRef types[1] = {lb_type(p->module, platform_type)};
			lbValue res = {};

			LLVMValueRef args[3] = {
					x.value,
					y.value,
					scale.value };

			res.value = lb_call_intrinsic(p, name, args, gb_count_of(args), types, gb_count_of(types));
			res.type = platform_type;
			return lb_emit_conv(p, res, tv.type);
		}

	case BuiltinProc_expect:
		{
			Type *t = default_type(tv.type);
			lbValue x = lb_emit_conv(p, lb_build_expr(p, ce->args[0]), t);
			lbValue y = lb_emit_conv(p, lb_build_expr(p, ce->args[1]), t);

			char const *name = "llvm.expect";

			LLVMTypeRef types[1] = {lb_type(p->module, t)};
			lbValue res = {};
			LLVMValueRef args[2] = { x.value, y.value };

			res.value = lb_call_intrinsic(p, name, args, gb_count_of(args), types, gb_count_of(types));
			res.type = t;
			return lb_emit_conv(p, res, t);
		}

	case BuiltinProc_prefetch_read_instruction:
	case BuiltinProc_prefetch_read_data:
	case BuiltinProc_prefetch_write_instruction:
	case BuiltinProc_prefetch_write_data:
		{
			lbValue ptr = lb_emit_conv(p, lb_build_expr(p, ce->args[0]), t_rawptr);
			unsigned long long locality = cast(unsigned long long)exact_value_to_i64(ce->args[1]->tav.value);
			unsigned long long rw = 0;
			unsigned long long cache = 0;
			switch (id) {
			case BuiltinProc_prefetch_read_instruction:
				rw = 0;
				cache = 0;
				break;
			case BuiltinProc_prefetch_read_data:
				rw = 0;
				cache = 1;
				break;
			case BuiltinProc_prefetch_write_instruction:
				rw = 1;
				cache = 0;
				break;
			case BuiltinProc_prefetch_write_data:
				rw = 1;
				cache = 1;
				break;
			}

			char const *name = "llvm.prefetch";

			LLVMTypeRef types[1] = {lb_type(p->module, t_rawptr)};

			LLVMTypeRef llvm_i32 = lb_type(p->module, t_i32);
			LLVMValueRef args[4] = {};
			args[0] = ptr.value;
			args[1] = LLVMConstInt(llvm_i32, rw, false);
			args[2] = LLVMConstInt(llvm_i32, locality, false);
			args[3] = LLVMConstInt(llvm_i32, cache, false);

			lbValue res = {};
			res.value = lb_call_intrinsic(p, name, args, gb_count_of(args), types, gb_count_of(types));
			res.type = nullptr;
			return res;
		}

	case BuiltinProc___entry_point:
		if (p->module->info->entry_point) {
			lbValue entry_point = lb_find_procedure_value_from_entity(p->module, p->module->info->entry_point);
			GB_ASSERT(entry_point.value != nullptr);
			lb_emit_call(p, entry_point, {});
		}
		return {};

	case BuiltinProc_syscall:
		{
			unsigned arg_count = cast(unsigned)ce->args.count;
			LLVMValueRef *args = gb_alloc_array(permanent_allocator(), LLVMValueRef, arg_count);
			for_array(i, ce->args) {
				lbValue arg = lb_build_expr(p, ce->args[i]);
				arg = lb_emit_conv(p, arg, t_uintptr);
				args[i] = arg.value;
			}

			LLVMTypeRef llvm_uintptr = lb_type(p->module, t_uintptr);
			LLVMTypeRef *llvm_arg_types = gb_alloc_array(permanent_allocator(), LLVMTypeRef, arg_count);
			for (unsigned i = 0; i < arg_count; i++) {
				llvm_arg_types[i] = llvm_uintptr;
			}

			LLVMTypeRef func_type = LLVMFunctionType(llvm_uintptr, llvm_arg_types, arg_count, false);

			LLVMValueRef inline_asm = nullptr;

			switch (build_context.metrics.arch) {
			case TargetArch_riscv64:
				{
					GB_ASSERT(arg_count <= 7);

					char asm_string[] = "ecall";
					gbString constraints = gb_string_make(heap_allocator(), "={a0}");
					for (unsigned i = 0; i < arg_count; i++) {
						constraints = gb_string_appendc(constraints, ",{");
						static char const *regs[] = {
							"a7",
							"a0",
							"a1",
							"a2",
							"a3",
							"a4",
							"a5",
							"a6"
						};
						constraints = gb_string_appendc(constraints, regs[i]);
						constraints = gb_string_appendc(constraints, "}");
					}

					constraints = gb_string_appendc(constraints, ",~{memory}");

					inline_asm = llvm_get_inline_asm(func_type, make_string_c(asm_string), make_string_c(constraints));
				}
				break;
			case TargetArch_amd64:
				{
					GB_ASSERT(arg_count <= 7);

					char asm_string[] = "syscall";
					gbString constraints = gb_string_make(heap_allocator(), "={rax}");
					for (unsigned i = 0; i < arg_count; i++) {
						constraints = gb_string_appendc(constraints, ",{");
						static char const *regs[] = {
							"rax",
							"rdi",
							"rsi",
							"rdx",
							"r10",
							"r8",
							"r9"
						};
						constraints = gb_string_appendc(constraints, regs[i]);
						constraints = gb_string_appendc(constraints, "}");
					}

					// The SYSCALL instruction stores the address of the
					// following instruction into RCX, and RFLAGS in R11.
					//
					// RSP is not saved, but at least on Linux it appears
					// that the kernel system-call handler does the right
					// thing.
					//
					// Some but not all system calls will additionally
					// clobber memory.
					//
					// TODO:
					//  * Figure out what Darwin does.
					constraints = gb_string_appendc(constraints, ",~{rcx},~{r11},~{memory}");
					inline_asm = llvm_get_inline_asm(func_type, make_string_c(asm_string), make_string_c(constraints));
				}
				break;
			case TargetArch_i386:
				{
					GB_ASSERT(arg_count <= 7);

					char asm_string[] = "int $$0x80";
					gbString constraints = gb_string_make(heap_allocator(), "={eax}");

					for (unsigned i = 0; i < gb_min(arg_count, 6); i++) {
						constraints = gb_string_appendc(constraints, ",{");
						static char const *regs[] = {
							"eax",
							"ebx",
							"ecx",
							"edx",
							"esi",
							"edi",
							"ebp",
						};
						constraints = gb_string_appendc(constraints, regs[i]);
						constraints = gb_string_appendc(constraints, "}");
					}

					constraints = gb_string_appendc(constraints, ",~{memory}");

					inline_asm = llvm_get_inline_asm(func_type, make_string_c(asm_string), make_string_c(constraints));
				}
				break;
			case TargetArch_arm64:
				{
					GB_ASSERT(arg_count <= 7);

					if(build_context.metrics.os == TargetOs_darwin) {
						char asm_string[] = "svc #0x80";
						gbString constraints = gb_string_make(heap_allocator(), "={x0}");
						for (unsigned i = 0; i < arg_count; i++) {
							constraints = gb_string_appendc(constraints, ",{");
							static char const *regs[] = {
								"x16",
								"x0",
								"x1",
								"x2",
								"x3",
								"x4",
								"x5",
							};
							constraints = gb_string_appendc(constraints, regs[i]);
							constraints = gb_string_appendc(constraints, "}");
						}

						constraints = gb_string_appendc(constraints, ",~{memory}");

						inline_asm = llvm_get_inline_asm(func_type, make_string_c(asm_string), make_string_c(constraints));
					} else {
						char asm_string[] = "svc #0";
						gbString constraints = gb_string_make(heap_allocator(), "={x0}");
						for (unsigned i = 0; i < arg_count; i++) {
							constraints = gb_string_appendc(constraints, ",{");
							static char const *regs[] = {
								"x8",
								"x0",
								"x1",
								"x2",
								"x3",
								"x4",
								"x5",
							};
							constraints = gb_string_appendc(constraints, regs[i]);
							constraints = gb_string_appendc(constraints, "}");
						}

						constraints = gb_string_appendc(constraints, ",~{memory}");

						inline_asm = llvm_get_inline_asm(func_type, make_string_c(asm_string), make_string_c(constraints));
					}
				}
				break;
			case TargetArch_arm32:
				{
					GB_ASSERT(arg_count <= 7);

					char asm_string[] = "svc #0";
					gbString constraints = gb_string_make(heap_allocator(), "={r0}");
					for (unsigned i = 0; i < arg_count; i++) {
						constraints = gb_string_appendc(constraints, ",{");
						static char const *regs[] = {
							"r7",
							"r0",
							"r1",
							"r2",
							"r3",
							"r4",
							"r5",
							"r6",
						};
						constraints = gb_string_appendc(constraints, regs[i]);
						constraints = gb_string_appendc(constraints, "}");
					}

					constraints = gb_string_appendc(constraints, ",~{memory}");

					inline_asm = llvm_get_inline_asm(func_type, make_string_c(asm_string), make_string_c(constraints));
				}
				break;
			default:
				GB_PANIC("Unsupported platform");
			}
			
			lbValue res = {};
			res.value = LLVMBuildCall2(p->builder, func_type, inline_asm, args, arg_count, "");
			res.type = t_uintptr;
			return res;
		}
	case BuiltinProc_syscall_bsd:
		{
			// This is a BSD-style syscall where errors are indicated by a high
			// Carry Flag and a positive return value, allowing the kernel to
			// return any value that fits into a machine word.
			//
			// This is unlike Linux, where errors are indicated by a negative
			// return value, limiting what can be expressed in one result.
			unsigned arg_count = cast(unsigned)ce->args.count;
			LLVMValueRef *args = gb_alloc_array(permanent_allocator(), LLVMValueRef, arg_count);
			for_array(i, ce->args) {
				lbValue arg = lb_build_expr(p, ce->args[i]);
				arg = lb_emit_conv(p, arg, t_uintptr);
				args[i] = arg.value;
			}

			LLVMTypeRef llvm_uintptr = lb_type(p->module, t_uintptr);
			LLVMTypeRef *llvm_arg_types = gb_alloc_array(permanent_allocator(), LLVMTypeRef, arg_count);
			for (unsigned i = 0; i < arg_count; i++) {
				llvm_arg_types[i] = llvm_uintptr;
			}

			LLVMTypeRef *results = gb_alloc_array(permanent_allocator(), LLVMTypeRef, 2);
			results[0] = lb_type(p->module, t_uintptr);
			results[1] = lb_type(p->module, t_bool);
			LLVMTypeRef llvm_results = LLVMStructTypeInContext(p->module->ctx, results, 2, false);

			LLVMTypeRef func_type = LLVMFunctionType(llvm_results, llvm_arg_types, arg_count, false);

			LLVMValueRef inline_asm = nullptr;

			switch (build_context.metrics.arch) {
			case TargetArch_amd64:
				{
					GB_ASSERT(arg_count <= 7);

					char asm_string[] = "syscall; setnb %cl";

					// Using CL as an output; RCX doesn't need to get clobbered later.
					gbString constraints = gb_string_make(heap_allocator(), "={rax},={cl}");
					for (unsigned i = 0; i < arg_count; i++) {
						constraints = gb_string_appendc(constraints, ",{");
						static char const *regs[] = {
							"rax",
							"rdi",
							"rsi",
							"rdx",
							"r10",
							"r8",
							"r9",
						};
						constraints = gb_string_appendc(constraints, regs[i]);
						constraints = gb_string_appendc(constraints, "}");
					}

					// NOTE(Feoramund): If you're experiencing instability
					// regarding syscalls during optimized builds, it is
					// possible that the ABI has changed for your platform,
					// or I've missed a register clobber.
					//
					// Documentation on this topic is sparse, but I was able to
					// determine what registers were being clobbered by adding
					// dummy values to them, setting a breakpoint after the
					// syscall, and checking the state of the registers afterwards.
					//
					// Be advised that manually stepping through a debugger may
					// cause the kernel to not return via sysret, which will
					// preserve register state that normally would've been
					// otherwise clobbered.
					//
					// It is also possible that some syscalls clobber different registers.

					if (build_context.metrics.os == TargetOs_freebsd) {
						// As a fix for CVE-2019-5595, FreeBSD started
						// clobbering R8, R9, and R10, instead of restoring
						// them.
						//
						// More info here:
						//
						// https://www.freebsd.org/security/advisories/FreeBSD-SA-19:01.syscall.asc
						// https://github.com/freebsd/freebsd-src/blob/098dbd7ff7f3da9dda03802cdb2d8755f816eada/sys/amd64/amd64/exception.S#L605
						// https://stackoverflow.com/q/66878250
						constraints = gb_string_appendc(constraints, ",~{r8},~{r9},~{r10}");
					}

					// Both FreeBSD and NetBSD might clobber RDX.
					//
					// For NetBSD, it was clobbered during a call to sysctl.
					//
					// For FreeBSD, it's listed as "return value 2" in their
					// AMD64 assembly, so there's no guarantee that it will persist.
					constraints = gb_string_appendc(constraints, ",~{rdx},~{r11},~{cc},~{memory}");

					inline_asm = llvm_get_inline_asm(func_type, make_string_c(asm_string), make_string_c(constraints));
				}
				break;
			case TargetArch_arm64:
				{
					GB_ASSERT(arg_count <= 7);

					char const *asm_string;
					char const **regs;
					gbString constraints;

					if (build_context.metrics.os == TargetOs_netbsd) {
						asm_string = "svc #0; cset x17, cc";
						constraints = gb_string_make(heap_allocator(), "={x0},={x17}");
						static char const *_regs[] = {
							"x17",
							"x0",
							"x1",
							"x2",
							"x3",
							"x4",
							"x5",
						};
						regs = _regs;
					} else {
						// FreeBSD (tested), OpenBSD (untested).
						asm_string = "svc #0; cset x8, cc";
						constraints = gb_string_make(heap_allocator(), "={x0},={x8}");
						static char const *_regs[] = {
							"x8",
							"x0",
							"x1",
							"x2",
							"x3",
							"x4",
							"x5",
						};
						regs = _regs;

						// FreeBSD clobbered x1 on a call to sysctl.
						constraints = gb_string_appendc(constraints, ",~{x1}");
					}

					for (unsigned i = 0; i < arg_count; i++) {
						constraints = gb_string_appendc(constraints, ",{");
						constraints = gb_string_appendc(constraints, regs[i]);
						constraints = gb_string_appendc(constraints, "}");
					}

					constraints = gb_string_appendc(constraints, ",~{cc},~{memory}");
					inline_asm = llvm_get_inline_asm(func_type, make_string_c(asm_string), make_string_c(constraints));
				}
				break;
			default:
				GB_PANIC("Unsupported platform");
			}
			
 			lbValue res = {};
 			res.value = LLVMBuildCall2(p->builder, func_type, inline_asm, args, arg_count, "");
			res.type = make_optional_ok_type(t_uintptr, true);

			return res;
		}

	case BuiltinProc_objc_send:
		return lb_handle_objc_send(p, expr);

	case BuiltinProc_objc_find_selector:     return lb_handle_objc_find_selector(p, expr);
	case BuiltinProc_objc_find_class:        return lb_handle_objc_find_class(p, expr);
	case BuiltinProc_objc_register_selector: return lb_handle_objc_register_selector(p, expr);
	case BuiltinProc_objc_register_class:    return lb_handle_objc_register_class(p, expr);
	case BuiltinProc_objc_ivar_get:          return lb_handle_objc_ivar_get(p, expr);


	case BuiltinProc_constant_utf16_cstring:
		{
			auto const encode_surrogate_pair = [](Rune r, u16 *r1, u16 *r2) {
				if (r < 0x10000 || r > 0x10ffff) {
					*r1 = 0xfffd;
					*r2 = 0xfffd;
				} else {
					r -= 0x10000;
					*r1 = 0xd800 + ((r>>10)&0x3ff);
					*r2 = 0xdc00 + (r&0x3ff);
				}
			};

			lbModule *m = p->module;

			auto tav = type_and_value_of_expr(ce->args[0]);
			GB_ASSERT(tav.value.kind == ExactValue_String);
			String value = tav.value.value_string;

			LLVMTypeRef llvm_u16 = lb_type(m, t_u16);

			isize max_len = value.len*2 + 1;
			LLVMValueRef *buffer = gb_alloc_array(temporary_allocator(), LLVMValueRef, max_len);
			isize n = 0;
			while (value.len > 0) {
				Rune r = 0;
				isize w = gb_utf8_decode(value.text, value.len, &r);
				value.text += w;
				value.len  -= w;
				if ((0 <= r && r < 0xd800) || (0xe000 <= r && r < 0x10000)) {
					buffer[n++] = LLVMConstInt(llvm_u16, cast(u16)r, false);
				} else if (0x10000 <= r && r <= 0x10ffff) {
					u16 r1, r2;
					encode_surrogate_pair(r, &r1, &r2);
					buffer[n++] = LLVMConstInt(llvm_u16, r1, false);
					buffer[n++] = LLVMConstInt(llvm_u16, r2, false);
				} else {
					buffer[n++] = LLVMConstInt(llvm_u16, 0xfffd, false);
				}
			}

			buffer[n++] = LLVMConstInt(llvm_u16, 0, false);

			LLVMValueRef array = LLVMConstArray(llvm_u16, buffer, cast(unsigned int)n);

			char *name = nullptr;
			{
				isize max_len = 7+8+1;
				name = gb_alloc_array(permanent_allocator(), char, max_len);
				u32 id = m->global_array_index.fetch_add(1);
				isize len = gb_snprintf(name, max_len, "csbs$%x", id);
				len -= 1;
			}
			LLVMTypeRef type = LLVMTypeOf(array);
			LLVMValueRef global_data = LLVMAddGlobal(m->mod, type, name);
			LLVMSetInitializer(global_data, array);
			LLVMSetUnnamedAddress(global_data, LLVMGlobalUnnamedAddr);
			LLVMSetLinkage(global_data, LLVMInternalLinkage);



			LLVMValueRef indices[] = {
				LLVMConstInt(lb_type(m, t_u32), 0, false),
				LLVMConstInt(lb_type(m, t_u32), 0, false),
			};
			lbValue res = {};
			res.type = tv.type;
			res.value = LLVMBuildInBoundsGEP2(p->builder, type, global_data, indices, gb_count_of(indices), "");
			return res;

		}

	case BuiltinProc_wasm_memory_grow:
		{
			char const *name = "llvm.wasm.memory.grow";
			LLVMTypeRef types[1] = {
				lb_type(p->module, t_i32),
			};

			LLVMValueRef args[2] = {};
			args[0] = lb_emit_conv(p, lb_build_expr(p, ce->args[0]), t_uintptr).value;
			args[1] = lb_emit_conv(p, lb_build_expr(p, ce->args[1]), t_uintptr).value;

			lbValue res = {};
			res.type = t_i32;
			res.value = lb_call_intrinsic(p, name, args, gb_count_of(args), types, gb_count_of(types));
			return lb_emit_conv(p, res, tv.type);
		}
	case BuiltinProc_wasm_memory_size:
		{
			char const *name = "llvm.wasm.memory.size";
			LLVMTypeRef types[1] = {
				lb_type(p->module, t_i32),
			};

			LLVMValueRef args[1] = {};
			args[0] = lb_emit_conv(p, lb_build_expr(p, ce->args[0]), t_uintptr).value;

			lbValue res = {};
			res.type = t_i32;
			res.value = lb_call_intrinsic(p, name, args, gb_count_of(args), types, gb_count_of(types));
			return lb_emit_conv(p, res, tv.type);
		}

	case BuiltinProc_wasm_memory_atomic_wait32:
		{
			char const *name = "llvm.wasm.memory.atomic.wait32";

			Type *t_u32_ptr = alloc_type_pointer(t_u32);

			LLVMValueRef args[3] = {};
			args[0] = lb_emit_conv(p, lb_build_expr(p, ce->args[0]), t_u32_ptr).value;
			args[1] = lb_emit_conv(p, lb_build_expr(p, ce->args[1]), t_u32).value;
			args[2] = lb_emit_conv(p, lb_build_expr(p, ce->args[2]), t_i64).value;

			lbValue res = {};
			res.type = tv.type;
			res.value = lb_call_intrinsic(p, name, args, gb_count_of(args), nullptr, 0);
			return res;
		}

	case BuiltinProc_wasm_memory_atomic_notify32:
		{
			char const *name = "llvm.wasm.memory.atomic.notify";

			Type *t_u32_ptr = alloc_type_pointer(t_u32);

			LLVMValueRef args[2] = {
				lb_emit_conv(p, lb_build_expr(p, ce->args[0]), t_u32_ptr).value,
				lb_emit_conv(p, lb_build_expr(p, ce->args[1]), t_u32).value
			};

			lbValue res = {};
			res.type = tv.type;
			res.value = lb_call_intrinsic(p, name, args, gb_count_of(args), nullptr, 0);
			return res;
		}


	case BuiltinProc_x86_cpuid:
		{
			Type *param_types[2] = {t_u32, t_u32};
			Type *type = alloc_type_proc_from_types(param_types, gb_count_of(param_types), tv.type, false, ProcCC_None);
			LLVMTypeRef func_type = lb_get_procedure_raw_type(p->module, type);
			LLVMValueRef the_asm = llvm_get_inline_asm(
				func_type,
				str_lit("cpuid"),
				str_lit("={ax},={bx},={cx},={dx},{ax},{cx}"),
				true
			);
			GB_ASSERT(the_asm != nullptr);

			LLVMValueRef args[2] = {};
			args[0] = lb_emit_conv(p, lb_build_expr(p, ce->args[0]), t_u32).value;
			args[1] = lb_emit_conv(p, lb_build_expr(p, ce->args[1]), t_u32).value;
			lbValue res = {};
			res.type = tv.type;
			res.value = LLVMBuildCall2(p->builder, func_type, the_asm, args, gb_count_of(args), "");
			return res;
		}
	case BuiltinProc_x86_xgetbv:
		{
			Type *type = alloc_type_proc_from_types(&t_u32, 1, tv.type, false, ProcCC_None);
			LLVMTypeRef func_type = lb_get_procedure_raw_type(p->module, type);
			LLVMValueRef the_asm = llvm_get_inline_asm(
				func_type,
				str_lit("xgetbv"),
				str_lit("={ax},={dx},{cx}"),
				true
			);
			GB_ASSERT(the_asm != nullptr);

			LLVMValueRef args[1] = {};
			args[0] = lb_emit_conv(p, lb_build_expr(p, ce->args[0]), t_u32).value;
			lbValue res = {};
			res.type = tv.type;
			res.value = LLVMBuildCall2(p->builder, func_type, the_asm, args, gb_count_of(args), "");
			return res;
		}

	case BuiltinProc_valgrind_client_request:
		{
			lbValue args[7] = {};
			for (isize i = 0; i < 7; i++) {
				args[i] = lb_emit_conv(p, lb_build_expr(p, ce->args[i]), t_uintptr);
			}
			if (!build_context.ODIN_VALGRIND_SUPPORT) {
				return args[0];
			}
			lbValue array = lb_generate_local_array(p, t_uintptr, 6, false);
			for (isize i = 0; i < 6; i++) {
				lbValue gep = lb_emit_array_epi(p, array, i);
				lb_emit_store(p, gep, args[i+1]);
			}

			switch (build_context.metrics.arch) {
			case TargetArch_amd64:
				{
					Type *param_types[2] = {};
					param_types[0] = t_uintptr;
					param_types[1] = array.type;

					Type *type = alloc_type_proc_from_types(param_types, gb_count_of(param_types), t_uintptr, false, ProcCC_None);
					LLVMTypeRef func_type = lb_get_procedure_raw_type(p->module, type);
					LLVMValueRef the_asm = llvm_get_inline_asm(
						func_type,
						str_lit("rolq $$3, %rdi; rolq $$13, %rdi\n rolq $$61, %rdi; rolq $$51, %rdi\n xchgq %rbx, %rbx"),
						str_lit("={rdx},{rdx},{rax},~{cc},~{memory}"),
						true
					);

					LLVMValueRef asm_args[2] = {};
					asm_args[0] = args[0].value;
					asm_args[1] = array.value;

					lbValue res = {};
					res.type = t_uintptr;
					res.value = LLVMBuildCall2(p->builder, func_type, the_asm, asm_args, gb_count_of(asm_args), "");
					return res;
				}
				break;
			default:
				GB_PANIC("Unsupported architecture: %.*s", LIT(target_arch_names[build_context.metrics.arch]));
				break;
			}

		}

	}

	GB_PANIC("Unhandled built-in procedure %.*s", LIT(builtin_procs[id].name));
	return {};
}


gb_internal lbValue lb_handle_param_value(lbProcedure *p, Type *parameter_type, ParameterValue const &param_value, TypeProc *procedure_type, Ast* call_expression) {
	switch (param_value.kind) {
	case ParameterValue_Constant:
		if (is_type_constant_type(parameter_type)) {
			auto res = lb_const_value(p->module, parameter_type, param_value.value);
			return res;
		} else {
			ExactValue ev = param_value.value;
			lbValue arg = {};
			Type *type = type_of_expr(param_value.original_ast_expr);
			if (type != nullptr) {
				arg = lb_const_value(p->module, type, ev);
			} else {
				arg = lb_const_value(p->module, parameter_type, param_value.value);
			}
			return lb_emit_conv(p, arg, parameter_type);
		}

	case ParameterValue_Nil:
		return lb_const_nil(p->module, parameter_type);
	case ParameterValue_Location:
		{
			String proc_name = {};
			if (p->entity != nullptr) {
				proc_name = p->entity->token.string;
			}

			ast_node(ce, CallExpr, call_expression);
			TokenPos pos = ast_token(ce->proc).pos;

			return lb_emit_source_code_location_as_global(p, proc_name, pos);
		}
	case ParameterValue_Expression:
		{
			Ast *orig = param_value.original_ast_expr;
			if (orig->kind == Ast_BasicDirective) {
				gbString expr = expr_to_string(call_expression, temporary_allocator());
				return lb_const_string(p->module, make_string_c(expr));
			}

			isize param_idx = -1;
			String param_str = {0};
			{
				Ast *call = unparen_expr(orig);
				GB_ASSERT(call->kind == Ast_CallExpr);
				ast_node(ce, CallExpr, call);
				GB_ASSERT(ce->proc->kind == Ast_BasicDirective);
				GB_ASSERT(ce->args.count == 1);
				Ast *target = ce->args[0];
				GB_ASSERT(target->kind == Ast_Ident);
				String target_str = target->Ident.token.string;

				param_idx = lookup_procedure_parameter(procedure_type, target_str);
				param_str = target_str;
			}
			GB_ASSERT(param_idx >= 0);


			Ast *target_expr = nullptr;
			ast_node(ce, CallExpr, call_expression);

			if (ce->split_args->positional.count > param_idx) {
				target_expr = ce->split_args->positional[param_idx];
			}

			for_array(i, ce->split_args->named) {
				Ast *arg = ce->split_args->named[i];
				ast_node(fv, FieldValue, arg);
				GB_ASSERT(fv->field->kind == Ast_Ident);
				String name = fv->field->Ident.token.string;
				if (name == param_str) {
					target_expr = fv->value;
					break;
				}
			}

			gbString expr = expr_to_string(target_expr, temporary_allocator());
			return lb_const_string(p->module, make_string_c(expr));
		}

	case ParameterValue_Value:
		return lb_build_expr(p, param_value.ast_value);
	}
	return lb_const_nil(p->module, parameter_type);
}


gb_internal lbValue lb_build_call_expr_internal(lbProcedure *p, Ast *expr);

gb_internal lbValue lb_build_call_expr(lbProcedure *p, Ast *expr) {
	expr = unparen_expr(expr);
	ast_node(ce, CallExpr, expr);

	lbValue res = lb_build_call_expr_internal(p, expr);

	if (ce->optional_ok_one) {
		GB_ASSERT(is_type_tuple(res.type));
		GB_ASSERT(res.type->Tuple.variables.count == 2);
		return lb_emit_struct_ev(p, res, 0);
	}
	return res;
}

gb_internal void lb_add_values_to_array(lbProcedure *p, Array<lbValue> *args, lbValue value) {
	if (is_type_tuple(value.type)) {
		for_array(i, value.type->Tuple.variables) {
			lbValue sub_value = lb_emit_struct_ev(p, value, cast(i32)i);
			array_add(args, sub_value);
		}
	} else {
		array_add(args, value);
	}
}

gb_internal lbValue lb_build_call_expr_internal(lbProcedure *p, Ast *expr) {
	lbModule *m = p->module;

	TypeAndValue tv = type_and_value_of_expr(expr);

	ast_node(ce, CallExpr, expr);

	TypeAndValue proc_tv = type_and_value_of_expr(ce->proc);
	AddressingMode proc_mode = proc_tv.mode;
	if (proc_mode == Addressing_Type) {
		GB_ASSERT(ce->args.count == 1);
		lbValue x = lb_build_expr(p, ce->args[0]);
		lbValue y = lb_emit_conv(p, x, tv.type);
		y.type = tv.type;
		return y;
	}

	Ast *proc_expr = unparen_expr(ce->proc);
	if (proc_mode == Addressing_Builtin) {
		Entity *e = entity_of_node(proc_expr);
		BuiltinProcId id = BuiltinProc_Invalid;
		if (e != nullptr) {
			id = cast(BuiltinProcId)e->Builtin.id;
		} else {
			id = BuiltinProc_DIRECTIVE;
		}
		return lb_build_builtin_proc(p, expr, tv, id);
	}

	// NOTE(bill): Regular call
	lbValue value = {};

	Entity *proc_entity = entity_of_node(proc_expr);
	if (proc_entity != nullptr) {
		if (proc_entity->flags & EntityFlag_Disabled) {
			GB_ASSERT(tv.type == nullptr);
			return {};
		}
	}

	if (proc_expr->tav.mode == Addressing_Constant) {
		ExactValue v = proc_expr->tav.value;
		switch (v.kind) {
		case ExactValue_Integer:
			{
				u64 u = big_int_to_u64(&v.value_integer);
				lbValue x = {};
				x.value = LLVMConstInt(lb_type(m, t_uintptr), u, false);
				x.type = t_uintptr;
				x = lb_emit_conv(p, x, t_rawptr);
				value = lb_emit_conv(p, x, proc_expr->tav.type);
				break;
			}
		case ExactValue_Pointer:
			{
				u64 u = cast(u64)v.value_pointer;
				lbValue x = {};
				x.value = LLVMConstInt(lb_type(m, t_uintptr), u, false);
				x.type = t_uintptr;
				x = lb_emit_conv(p, x, t_rawptr);
				value = lb_emit_conv(p, x, proc_expr->tav.type);
				break;
			}
		}
	}

	if (value.value == nullptr) {
		value = lb_build_expr(p, proc_expr);
	}

	GB_ASSERT(value.value != nullptr);
	Type *proc_type_ = base_type(value.type);
	GB_ASSERT(proc_type_->kind == Type_Proc);
	TypeProc *pt = &proc_type_->Proc;

	GB_ASSERT(ce->split_args != nullptr);

	auto args = array_make<lbValue>(permanent_allocator(), 0, pt->param_count);

	bool vari_expand = (ce->ellipsis.pos.line != 0);
	bool is_c_vararg = pt->c_vararg;

	for_array(i, ce->split_args->positional) {
		Entity *e = pt->params->Tuple.variables[i];
		if (e->kind == Entity_TypeName) {
			array_add(&args, lb_const_nil(p->module, e->type));
			continue;
		} else if (e->kind == Entity_Constant) {
			array_add(&args, lb_const_value(p->module, e->type, e->Constant.value));
			continue;
		}

		GB_ASSERT(e->kind == Entity_Variable);

		if (pt->variadic && pt->variadic_index == i) {
			lbValue variadic_args = lb_const_nil(p->module, e->type);
			auto variadic = slice(ce->split_args->positional, pt->variadic_index, ce->split_args->positional.count);
			if (variadic.count != 0) {
				// variadic call argument generation
				Type *slice_type = e->type;
				GB_ASSERT(slice_type->kind == Type_Slice);

				if (is_c_vararg) {
					GB_ASSERT(!vari_expand);

					Type *elem_type = slice_type->Slice.elem;

					for (Ast *var_arg : variadic) {
						lbValue arg = lb_build_expr(p, var_arg);
						if (is_type_any(elem_type)) {
							if (is_type_untyped_nil(arg.type)) {
								arg = lb_const_nil(p->module, t_rawptr);
							}
							array_add(&args, lb_emit_c_vararg(p, arg, arg.type));
						} else {
							array_add(&args, lb_emit_c_vararg(p, arg, elem_type));
						}
					}
					break;
				} else if (vari_expand) {
					GB_ASSERT(variadic.count == 1);
					variadic_args = lb_build_expr(p, variadic[0]);
					variadic_args = lb_emit_conv(p, variadic_args, slice_type);
				} else {
					Type *elem_type = slice_type->Slice.elem;

					auto var_args = array_make<lbValue>(heap_allocator(), 0, variadic.count);
					defer (array_free(&var_args));
					for (Ast *var_arg : variadic) {
						lbValue v = lb_build_expr(p, var_arg);
						lb_add_values_to_array(p, &var_args, v);
					}
					isize slice_len = var_args.count;
					if (slice_len > 0) {
						lbAddr slice = {};

						for (auto const &vr : p->variadic_reuses) {
							if (are_types_identical(vr.slice_type, slice_type)) {
								slice = vr.slice_addr;
								break;
							}
						}

						DeclInfo *d = decl_info_of_entity(p->entity);
						if (d != nullptr && slice.addr.value == nullptr) {
							for (auto const &vr : d->variadic_reuses) {
								if (are_types_identical(vr.slice_type, slice_type)) {
								#if LLVM_VERSION_MAJOR >= 13
									// NOTE(bill): No point wasting even more memory, just reuse this stack variable too
									if (p->variadic_reuses.count > 0) {
										slice = p->variadic_reuses[0].slice_addr;
									} else {
										slice = lb_add_local_generated(p, slice_type, true);
									}
									// NOTE(bill): Change the underlying type to match the specific type
									slice.addr.type = alloc_type_pointer(slice_type);
								#else
									slice = lb_add_local_generated(p, slice_type, true);
								#endif
									array_add(&p->variadic_reuses, lbVariadicReuseSlices{slice_type, slice});
									break;
								}
							}
						}

						lbValue base_array_ptr = p->variadic_reuse_base_array_ptr.addr;
						if (base_array_ptr.value == nullptr) {
							if (d != nullptr) {
								i64 max_bytes = d->variadic_reuse_max_bytes;
								i64 max_align = gb_max(d->variadic_reuse_max_align, 16);
								p->variadic_reuse_base_array_ptr = lb_add_local_generated(p, alloc_type_array(t_u8, max_bytes), true);
								lb_try_update_alignment(p->variadic_reuse_base_array_ptr.addr, cast(unsigned)max_align);
								base_array_ptr = p->variadic_reuse_base_array_ptr.addr;
							} else {
								base_array_ptr = lb_add_local_generated(p, alloc_type_array(elem_type, slice_len), true).addr;
							}
						}

						if (slice.addr.value == nullptr) {
							slice = lb_add_local_generated(p, slice_type, true);
						}

						GB_ASSERT(base_array_ptr.value != nullptr);
						GB_ASSERT(slice.addr.value != nullptr);

						base_array_ptr = lb_emit_conv(p, base_array_ptr, alloc_type_pointer(alloc_type_array(elem_type, slice_len)));

						for (isize i = 0; i < var_args.count; i++) {
							lbValue addr = lb_emit_array_epi(p, base_array_ptr, cast(i32)i);
							lbValue var_arg = var_args[i];
							var_arg = lb_emit_conv(p, var_arg, elem_type);
							lb_emit_store(p, addr, var_arg);
						}

						lbValue base_elem = lb_emit_array_epi(p, base_array_ptr, 0);
						lbValue len = lb_const_int(p->module, t_int, slice_len);
						lb_fill_slice(p, slice, base_elem, len);

						variadic_args = lb_addr_load(p, slice);
					}
				}
			}
			array_add(&args, variadic_args);

			break;
		} else {
			lbValue value = lb_build_expr(p, ce->split_args->positional[i]);
			lb_add_values_to_array(p, &args, value);
		}
	}

	if (!is_c_vararg) {
		array_resize(&args, pt->param_count);
	}

	for (Ast *arg : ce->split_args->named) {
		ast_node(fv, FieldValue, arg);
		GB_ASSERT(fv->field->kind == Ast_Ident);
		String name = fv->field->Ident.token.string;
		gb_unused(name);
		isize param_index = lookup_procedure_parameter(pt, name);
		GB_ASSERT(param_index >= 0);

		Entity *e = pt->params->Tuple.variables[param_index];
		if (e->kind == Entity_TypeName) {
			lbValue value = lb_const_nil(p->module, e->type);
			args[param_index] = value;
		} else if (is_c_vararg && pt->variadic && pt->variadic_index == param_index) {
			GB_ASSERT(param_index == pt->param_count-1);
			Type *slice_type = e->type;
			GB_ASSERT(slice_type->kind == Type_Slice);
			Type *elem_type = slice_type->Slice.elem;

			if (fv->value->kind == Ast_CompoundLit) {
				ast_node(literal, CompoundLit, fv->value);
				for (Ast *var_arg : literal->elems) {
					lbValue arg = lb_build_expr(p, var_arg);
					if (is_type_any(elem_type)) {
						if (is_type_untyped_nil(arg.type)) {
							arg = lb_const_nil(p->module, t_rawptr);
						}
						array_add(&args, lb_emit_c_vararg(p, arg, arg.type));
					} else {
						array_add(&args, lb_emit_c_vararg(p, arg, elem_type));
					}
				}
			} else {
				lbValue value = lb_build_expr(p, fv->value);
				GB_ASSERT(!is_type_tuple(value.type));
				array_add(&args, lb_emit_c_vararg(p, value, value.type));
			}
		} else {
			lbValue value = lb_build_expr(p, fv->value);
			GB_ASSERT(!is_type_tuple(value.type));
			args[param_index] = value;
		}
	}


	if (pt->params != nullptr)  {
		isize min_count = pt->params->Tuple.variables.count;
		if (is_c_vararg) {
			min_count -= 1;
		}
		GB_ASSERT(args.count >= min_count);
		for_array(arg_index, pt->params->Tuple.variables) {
			Entity *e = pt->params->Tuple.variables[arg_index];
			if (pt->variadic && arg_index == pt->variadic_index) {
				if (!is_c_vararg && args[arg_index].value == 0) {
					args[arg_index] = lb_const_nil(p->module, e->type);
				}
				continue;
			}

			lbValue arg = args[arg_index];
			if (arg.value == nullptr && arg.type == nullptr) {
				switch (e->kind) {
				case Entity_TypeName:
					args[arg_index] = lb_const_nil(p->module, e->type);
					break;
				case Entity_Variable:
					args[arg_index] = lb_handle_param_value(p, e->type, e->Variable.param_value, pt, expr);
					break;

				case Entity_Constant:
					args[arg_index] = lb_const_value(p->module, e->type, e->Constant.value);
					break;
				default:
					GB_PANIC("Unknown entity kind %.*s\n", LIT(entity_strings[e->kind]));
				}
			} else {
				args[arg_index] = lb_emit_conv(p, arg, e->type);
			}
		}
	}

	isize final_count = is_c_vararg ? args.count : pt->param_count;
	auto call_args = array_slice(args, 0, final_count);
	return lb_emit_call(p, value, call_args, ce->inlining);
}

