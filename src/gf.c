// This file is a part of Julia. License is MIT: http://julialang.org/license

/*
  Generic Functions
  . method table and lookup
  . GF constructor, add_method
  . dispatch
  . static parameter inference
  . method specialization, invoking type inference
*/
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "julia.h"
#include "julia_internal.h"
#ifndef _OS_WINDOWS_
#include <unistd.h>
#endif

// ::ANY has no effect if the number of overlapping methods is greater than this
#define MAX_UNSPECIALIZED_CONFLICTS 32

#ifdef __cplusplus
extern "C" {
#endif

/// ----- Handling for Julia callbacks ----- ///

int in_pure_callback;

JL_DLLEXPORT int8_t jl_is_in_pure_context(void)
{
    return in_pure_callback;
}

JL_DLLEXPORT void jl_trace_method(jl_method_t *m)
{
    assert(jl_is_method(m));
    m->traced = 1;
}

JL_DLLEXPORT void jl_untrace_method(jl_method_t *m)
{
    assert(jl_is_method(m));
    m->traced = 0;
}

JL_DLLEXPORT void jl_trace_linfo(jl_lambda_info_t *linfo)
{
    assert(jl_is_lambda_info(linfo));
    linfo->compile_traced = 1;
}

JL_DLLEXPORT void jl_untrace_linfo(jl_lambda_info_t *linfo)
{
    assert(jl_is_lambda_info(linfo));
    linfo->compile_traced = 0;
}

static tracer_cb jl_method_tracer = NULL;
JL_DLLEXPORT void jl_register_method_tracer(void (*callback)(jl_lambda_info_t *tracee))
{
    jl_method_tracer = (tracer_cb)callback;
}

tracer_cb jl_newmeth_tracer = NULL;
JL_DLLEXPORT void jl_register_newmeth_tracer(void (*callback)(jl_method_t *tracee))
{
    jl_newmeth_tracer = (tracer_cb)callback;
}

tracer_cb jl_linfo_tracer = NULL;
JL_DLLEXPORT void jl_register_linfo_tracer(void (*callback)(jl_lambda_info_t *tracee))
{
    jl_linfo_tracer = (tracer_cb)callback;
}

void jl_call_tracer(tracer_cb callback, jl_value_t *tracee)
{
    int last_in = in_pure_callback;
    JL_TRY {
        in_pure_callback = 1;
        callback(tracee);
        in_pure_callback = last_in;
    }
    JL_CATCH {
        in_pure_callback = last_in;
        jl_printf(JL_STDERR, "WARNING: tracer callback function threw an error:\n");
        jl_static_show(JL_STDERR, jl_exception_in_transit);
        jl_printf(JL_STDERR, "\n");
        jlbacktrace();
    }
}

/// ----- Definitions for various internal TypeMaps ----- ///

const struct jl_typemap_info method_defs = {
    0, &jl_method_type
};
const struct jl_typemap_info lambda_cache = {
    0, &jl_lambda_info_type
};
const struct jl_typemap_info tfunc_cache = {
    1, &jl_any_type
};

static int8_t jl_cachearg_offset(jl_methtable_t *mt)
{
    return (mt == jl_type_type_mt) ? 0 : 1;
}

/// ----- Insertion logic for special entries ----- ///

JL_DLLEXPORT jl_lambda_info_t *jl_get_specialized(jl_method_t *m, jl_tupletype_t *types, jl_svec_t *sp);

// get or create the LambdaInfo for a specialization
JL_DLLEXPORT jl_lambda_info_t *jl_specializations_get_linfo(jl_method_t *m, jl_tupletype_t *type, jl_svec_t *sparams)
{
    jl_typemap_entry_t *sf = jl_typemap_assoc_by_type(m->specializations, type, NULL, 1, /*subtype*/0, /*offs*/0);
    if (sf && jl_is_lambda_info(sf->func.value) && ((jl_lambda_info_t*)sf->func.value)->code != jl_nothing)
        return (jl_lambda_info_t*)sf->func.value;
    jl_lambda_info_t *li = jl_get_specialized(m, type, sparams);
    JL_GC_PUSH1(&li);
    // TODO: fuse lookup and insert steps
    jl_typemap_insert(&m->specializations, (jl_value_t*)m, type, jl_emptysvec, NULL, jl_emptysvec, (jl_value_t*)li, 0, &tfunc_cache, NULL);
    JL_GC_POP();
    return li;
}

JL_DLLEXPORT jl_value_t *jl_specializations_lookup(jl_method_t *m, jl_tupletype_t *type)
{
    jl_typemap_entry_t *sf = jl_typemap_assoc_by_type(m->specializations, type, NULL, 2, /*subtype*/0, /*offs*/0);
    if (!sf)
        return jl_nothing;
    return sf->func.value;
}

JL_DLLEXPORT jl_value_t *jl_methtable_lookup(jl_methtable_t *mt, jl_tupletype_t *type)
{
    jl_typemap_entry_t *sf = jl_typemap_assoc_by_type(mt->defs, type, NULL, 2, /*subtype*/0, /*offs*/0);
    if (!sf)
        return jl_nothing;
    return sf->func.value;
}

// ----- LambdaInfo specialization instantiation ----- //

JL_DLLEXPORT jl_method_t *jl_new_method_uninit(void);
static jl_function_t *jl_new_generic_function_with_supertype(jl_sym_t *name, jl_module_t *module, jl_datatype_t *st, int iskw);
jl_value_t *jl_mk_builtin_func(const char *name, jl_fptr_t fptr)
{
    jl_sym_t *sname = jl_symbol(name);
    jl_value_t *f = jl_new_generic_function_with_supertype(sname, jl_core_module, jl_builtin_type, 0);
    jl_lambda_info_t *li = jl_new_lambda_info_uninit();
    li->fptr = fptr;
    // TODO jb/functions: what should li->code be?
    li->code = jl_nothing; jl_gc_wb(li, li->code);
    li->def = jl_new_method_uninit();
    li->def->name = sname;
    li->def->lambda_template = li;
    li->def->ambig = jl_nothing;
    jl_methtable_t *mt = jl_gf_mtable(f);
    jl_typemap_insert(&mt->cache, (jl_value_t*)mt, jl_anytuple_type, jl_emptysvec, NULL, jl_emptysvec, (jl_value_t*)li, 0, &lambda_cache, NULL);
    return f;
}

jl_lambda_info_t *jl_get_unspecialized(jl_lambda_info_t *method)
{
    // one unspecialized version of a function can be shared among all cached specializations
    jl_method_t *def = method->def;
    if (method->unspecialized_ducttape != NULL)
        return method->unspecialized_ducttape;
    if (method->sparam_syms != jl_emptysvec) {
        if (def->needs_sparam_vals_ducttape == 2) {
            jl_array_t *code = (jl_array_t*)def->lambda_template->code;
            JL_GC_PUSH1(&code);
            if (!jl_typeis(code, jl_array_any_type))
                code = jl_uncompress_ast(def->lambda_template, code);
            size_t i, l = jl_array_len(code);
            def->needs_sparam_vals_ducttape = 0;
            for (i = 0; i < l; i++) {
                if (jl_has_intrinsics(method, jl_array_ptr_ref(code, i), def->module)) {
                    def->needs_sparam_vals_ducttape = 1;
                    break;
                }
            }
            JL_GC_POP();
        }
        if (def->needs_sparam_vals_ducttape) {
            method->unspecialized_ducttape = jl_get_specialized(def, method->specTypes, method->sparam_vals);
            jl_gc_wb(method, method->unspecialized_ducttape);
            method->unspecialized_ducttape->unspecialized_ducttape = method->unspecialized_ducttape;
            return method->unspecialized_ducttape;
        }
    }
    return def->lambda_template;
}

/*
  run type inference on lambda "li" for given argument types.
  if "li" has been inferred before but the IR was deleted, returns a
  new LambdaInfo with the IR reconstituted.
*/
jl_lambda_info_t *jl_type_infer(jl_lambda_info_t *li, int force)
{
#ifdef ENABLE_INFERENCE
    jl_module_t *mod = NULL;
    if (li->def != NULL)
        mod = li->def->module;
    static int inInference = 0;
    int lastIn = inInference;
    inInference = 1;
    if (jl_typeinf_func != NULL && (force ||
        (mod != jl_gf_mtable(jl_typeinf_func)->module &&
        (mod != jl_core_module || !lastIn)))) { // avoid any potential recursion in calling jl_typeinf_func on itself
        JL_LOCK(&codegen_lock); // Might GC
        assert(li->inInference == 0);
        jl_value_t *fargs[2];
        fargs[0] = (jl_value_t*)jl_typeinf_func;
        fargs[1] = (jl_value_t*)li;
#ifdef TRACE_INFERENCE
        jl_printf(JL_STDERR,"inference on ");
        jl_static_show_func_sig(JL_STDERR, (jl_value_t*)li->specTypes);
        jl_printf(JL_STDERR, "\n");
#endif
        li = (jl_lambda_info_t*)jl_apply(fargs, 2);
        assert(li->def || li->inInference == 0); // if this is toplevel expr, make sure inference finished
        JL_UNLOCK(&codegen_lock); // Might GC
    }
    inInference = lastIn;
#endif
    return li;
}

static int get_spec_unspec_list(jl_typemap_entry_t *l, void *closure)
{
    if (jl_is_lambda_info(l->func.value) && !l->func.linfo->inferred)
        jl_array_ptr_1d_push((jl_array_t*)closure, l->func.value);
    return 1;
}

static int get_method_unspec_list(jl_typemap_entry_t *def, void *closure)
{
    jl_typemap_visitor(def->func.method->specializations, get_spec_unspec_list, closure);
    return 1;
}

static void jl_reset_mt_caches(jl_module_t *m, jl_array_t *unspec)
{
    // removes all method caches
    size_t i;
    void **table = m->bindings.table;
    for(i=1; i < m->bindings.size; i+=2) {
        if (table[i] != HT_NOTFOUND) {
            jl_binding_t *b = (jl_binding_t*)table[i];
            if (b->owner == m && b->value && b->constp) {
                if (jl_is_datatype(b->value)) {
                    jl_typename_t *tn = ((jl_datatype_t*)b->value)->name;
                    if (tn->module == m && tn->name == b->name) {
                        jl_methtable_t *mt = tn->mt;
                        if (mt != NULL && (jl_value_t*)mt != jl_nothing) {
                            if (mt->defs.unknown != jl_nothing) // make sure not to reset builtin functions
                                mt->cache.unknown = jl_nothing;
                            jl_typemap_visitor(mt->defs, get_method_unspec_list, (void*)unspec);
                        }
                    }
                }
                else if (jl_is_module(b->value)) {
                    jl_module_t *child = (jl_module_t*)b->value;
                    if (child != m && child->parent == m && child->name == b->name) {
                        // this is the original/primary binding for the submodule
                        jl_reset_mt_caches((jl_module_t*)b->value, unspec);
                    }
                }
            }
        }
    }
}

jl_function_t *jl_typeinf_func=NULL;

JL_DLLEXPORT void jl_set_typeinf_func(jl_value_t *f)
{
    jl_typeinf_func = (jl_function_t*)f;
    // give type inference a chance to see all of these
    jl_array_t *unspec = jl_alloc_vec_any(0);
    JL_GC_PUSH1(&unspec);
    jl_reset_mt_caches(jl_main_module, unspec);
    size_t i, l;
    for (i = 0, l = jl_array_len(unspec); i < l; i++) {
        jl_type_infer((jl_lambda_info_t*)jl_array_ptr_ref(unspec, i), 1);
    }
    JL_GC_POP();
}

static int very_general_type(jl_value_t *t)
{
    return (t && (t==(jl_value_t*)jl_any_type || t == (jl_value_t*)jl_type_type ||
                  (jl_is_typevar(t) &&
                   ((jl_tvar_t*)t)->ub==(jl_value_t*)jl_any_type)));
}

jl_value_t *jl_nth_slot_type(jl_tupletype_t *sig, size_t i)
{
    size_t len = jl_datatype_nfields(sig);
    if (len == 0)
        return NULL;
    if (i < len-1)
        return jl_tparam(sig, i);
    if (jl_is_vararg_type(jl_tparam(sig,len-1)))
        return jl_tparam0(jl_tparam(sig,len-1));
    if (i == len-1)
        return jl_tparam(sig, i);
    return NULL;
}

// after intersection, the argument tuple type needs to be corrected to reflect the signature match
// that occurred, if the arguments contained a Type but the signature matched on the kind
static jl_tupletype_t *join_tsig(jl_tupletype_t *tt, jl_tupletype_t *sig)
{
    jl_svec_t *newparams = NULL;
    JL_GC_PUSH1(&newparams);
    int changed = 0;
    size_t i, np;
    for (i = 0, np = jl_nparams(tt); i < np; i++) {
        jl_value_t *elt = jl_tparam(tt, i);
        jl_value_t *newelt = NULL;
        jl_value_t *decl_i = jl_nth_slot_type(sig, i);

        if (jl_is_type_type(elt)) {
            // if the declared type was not Any or Union{Type, ...},
            // then the match must been with TypeConstructor or DataType
            // and the result of matching the type signature
            // needs to be corrected to the leaf type 'kind'
            jl_value_t *kind = jl_typeof(jl_tparam0(elt));
            if (jl_subtype(kind, decl_i, 0)) {
                if (!jl_subtype((jl_value_t*)jl_type_type, decl_i, 0)) {
                    // TypeConstructors are problematic because they can be alternate
                    // representations of any type. If we matched this method because
                    // it matched the leaf type TypeConstructor, then don't
                    // cache something different since that doesn't necessarily actually apply
                    //
                    // similarly, if we matched Type{T<:Any}::DataType,
                    // then we don't want to cache it that way
                    // since lookup will think we matched ::Type{T}
                    // and that is quite a different thing
                    newelt = kind;
                }
            }
        }
        // prepare to build a new type with the replacement above
        if (newelt) {
            if (!changed) {
                newparams = jl_svec_copy(tt->parameters);
                changed = 1;
            }
            jl_svecset(newparams, i, newelt);
        }
    }
    if (changed)
        tt = jl_apply_tuple_type(newparams);
    JL_GC_POP();
    return tt;
}

static jl_value_t *ml_matches(union jl_typemap_t ml, int offs,
                              jl_tupletype_t *type, int lim, int include_ambiguous);

static jl_lambda_info_t *cache_method(jl_methtable_t *mt, union jl_typemap_t *cache, jl_value_t *parent,
                                      jl_tupletype_t *type, // the specialized type signature for type lambda
                                      jl_tupletype_t *tt, // the original tupletype of the signature
                                      jl_typemap_entry_t *m, jl_svec_t *sparams)
{
    JL_LOCK(&codegen_lock); // Might GC
    size_t i;
    jl_tupletype_t *decl = m->sig;
    jl_method_t *definition = m->func.method;
    int8_t isstaged = definition->isstaged;
    int need_guard_entries = 0;
    int hasnewparams = 0;
    int makesimplesig = 0;
    jl_value_t *temp=NULL;
    jl_value_t *temp2=NULL;
    jl_value_t *temp3=NULL;
    jl_lambda_info_t *newmeth=NULL;
    jl_svec_t *newparams=NULL;
    jl_svec_t *limited=NULL;
    JL_GC_PUSH5(&temp, &temp2, &temp3, &newmeth, &newparams);
    size_t np = jl_nparams(type);
    newparams = jl_svec_copy(type->parameters);

    for (i=0; i < np; i++) {
        jl_value_t *elt = jl_tparam(type,i);
        jl_value_t *decl_i = jl_nth_slot_type(decl,i);
        if ((tt != type && elt != jl_tparam(tt, i)) || // if join_tsig made a swap
                is_kind(elt)) { // might see a kind if called at compile-time
            // kind slots always need guard entries (checking for subtypes of Type)
            need_guard_entries = 1;
            continue;
        }

        if (isstaged) {
            // staged functions can't be optimized
            continue;
        }

        // avoid specializing on an argument of type Tuple
        // unless matching a declared type of `::Type`
        if (jl_is_type_type(elt) && jl_is_tuple_type(jl_tparam0(elt)) &&
            (!jl_subtype(decl_i, (jl_value_t*)jl_type_type, 0) || is_kind(decl_i))) { // Type{Tuple{...}}
            elt = (jl_value_t*)jl_anytuple_type_type; // Type{T<:Tuple}
            jl_svecset(newparams, i, elt);
            hasnewparams = 1;
            need_guard_entries = 1;
        }

        int notcalled_func = (i>0 && i<=8 && !(definition->called&(1<<(i-1))) &&
                              jl_subtype(elt,(jl_value_t*)jl_function_type,0));
        if (decl_i == jl_ANY_flag) {
            // don't specialize on slots marked ANY
            jl_svecset(newparams, i, (jl_value_t*)jl_any_type);
            hasnewparams = 1;
            need_guard_entries = 1;
        }
        else if (notcalled_func && (decl_i == (jl_value_t*)jl_any_type ||
                                    decl_i == (jl_value_t*)jl_function_type ||
                                    (jl_is_uniontype(decl_i) && jl_svec_len(((jl_uniontype_t*)decl_i)->types)==2 &&
                                     jl_subtype((jl_value_t*)jl_function_type, decl_i, 0) &&
                                     jl_subtype((jl_value_t*)jl_datatype_type, decl_i, 0)))) {
            // and attempt to despecialize types marked Function, Callable, or Any
            // when called with a subtype of Function but is not called
            jl_svecset(newparams, i, (jl_value_t*)jl_function_type);
            makesimplesig = 1;
            hasnewparams = 1;
            need_guard_entries = 1;
        }
        else if (jl_is_type_type(elt) && jl_is_type_type(jl_tparam0(elt)) &&
                 // give up on specializing static parameters for Type{Type{Type{...}}}
                 (jl_is_type_type(jl_tparam0(jl_tparam0(elt))) ||
                  decl_i==NULL || !jl_has_typevars(decl_i))) {
            /*
              actual argument was Type{...}, we computed its type as
              Type{Type{...}}. we must avoid unbounded nesting here, so
              cache the signature as Type{T}, unless something more
              specific like Type{Type{Int32}} was actually declared.
              this can be determined using a type intersection.
            */
            if (i < jl_nparams(decl)) {
                jl_value_t *declt = jl_tparam(decl,i);
                // for T..., intersect with T
                if (jl_is_vararg_type(declt))
                    declt = jl_tparam0(declt);
                jl_value_t *di = jl_type_intersection(declt, (jl_value_t*)jl_typetype_type);
                assert(di != (jl_value_t*)jl_bottom_type);
                if (is_kind(di))
                    // issue #11355: DataType has a UID and so takes precedence in the cache
                    jl_svecset(newparams, i, (jl_value_t*)jl_typetype_type);
                else
                    jl_svecset(newparams, i, di);
                // TODO: recompute static parameter values, so in extreme cases we
                // can give `T=Type` instead of `T=Type{Type{Type{...`.
            }
            else {
                jl_svecset(newparams, i, (jl_value_t*)jl_typetype_type);
            }
            need_guard_entries = 1;
            hasnewparams = 1;
        }
        else if (jl_is_type_type(elt) && very_general_type(decl_i) &&
                 !jl_has_typevars(decl_i)) {
            /*
              here's a fairly simple heuristic: if this argument slot's
              declared type is general (Type, Any, or ANY),
              then don't specialize for every Type that got passed.

              Since every type x has its own type Type{x}, this would be
              excessive specialization for an Any slot.

              This may require guard entries due to other potential matches.
              In particular, TypeConstructors are problematic because they can
              be alternate representations of any type. Extensionally, TC == TC.body,
              but typeof(TC) != typeof(TC.body). This creates an ambiguity:
              Type{TC} is type-equal to Type{TC.body}, yet a slot
              x::TypeConstructor matches the first but not the second, while
              also matching all other TypeConstructors. This means neither
              Type{TC} nor TypeConstructor is more specific.
            */
            jl_svecset(newparams, i, jl_typetype_type);
            need_guard_entries = 1;
            hasnewparams = 1;
        }
    }

    // for varargs methods, only specialize up to max_args.
    // in general, here we want to find the biggest type that's not a
    // supertype of any other method signatures. so far we are conservative
    // and the types we find should be bigger.
    if (!isstaged && jl_nparams(type) > mt->max_args
        && jl_va_tuple_kind(decl) == JL_VARARG_UNBOUND) {
        size_t nspec = mt->max_args + 2;
        limited = jl_alloc_svec(nspec);
        temp3 = (jl_value_t*)limited;
        for(i=0; i < nspec-1; i++) {
            jl_svecset(limited, i, jl_svecref(newparams, i));
        }
        jl_value_t *lasttype = jl_svecref(newparams,i-1);
        // if all subsequent arguments are subtypes of lasttype, specialize
        // on that instead of decl. for example, if decl is
        // (Any...)
        // and type is
        // (Symbol, Symbol, Symbol)
        // then specialize as (Symbol...), but if type is
        // (Symbol, Int32, Expr)
        // then specialize as (Any...)
        //
        // note: this also protects the work join_tsig did to correct `types` for the
        // leaftype signatures TypeConstructor and DataType
        // (assuming those made an unlikely appearance in Varargs position)
        size_t j = i;
        int all_are_subtypes=1;
        for(; j < jl_svec_len(newparams); j++) {
            if (!jl_subtype(jl_svecref(newparams,j), lasttype, 0)) {
                all_are_subtypes = 0;
                break;
            }
        }
        if (all_are_subtypes) {
            // avoid Type{Type{...}}...
            if (jl_is_type_type(lasttype) && jl_is_type_type(jl_tparam0(lasttype)))
                lasttype = (jl_value_t*)jl_type_type;
            jl_svecset(limited, i, jl_wrap_vararg(lasttype, (jl_value_t*)NULL));
        }
        else {
            jl_value_t *lastdeclt = jl_tparam(decl,jl_nparams(decl)-1);
            int nsp = jl_svec_len(sparams);
            if (nsp > 0) {
                temp2 = (jl_value_t*)jl_alloc_svec_uninit(2*nsp);
                for(j=0; j < nsp; j++) {
                    if (j==0 && jl_is_typevar(m->tvars))
                        jl_svecset(temp2, 0, m->tvars);
                    else
                        jl_svecset(temp2, j*2, jl_svecref(m->tvars, j));
                    jl_svecset(temp2, j*2+1, jl_svecref(sparams,j));
                }
                lastdeclt = (jl_value_t*)jl_instantiate_type_with((jl_value_t*)lastdeclt,
                                                                  jl_svec_data(temp2), nsp);
            }
            jl_svecset(limited, i, lastdeclt);
        }
        newparams = limited;
        hasnewparams = 1;
        // now there is a problem: the widened signature is more
        // general than just the given arguments, so it might conflict
        // with another definition that doesn't have cache instances yet.
        // to fix this, we insert guard cache entries for all intersections
        // of this signature and definitions. those guard entries will
        // supersede this one in conflicted cases, alerting us that there
        // should actually be a cache miss.
        need_guard_entries = 1;
    }

    int cache_with_orig = 0;
    jl_svec_t* guardsigs = jl_emptysvec;
    jl_tupletype_t *origtype = type; // backup the prior value of `type`
    if (hasnewparams) {
        type = jl_apply_tuple_type(newparams);
        temp2 = (jl_value_t*)type;
    }
    if (need_guard_entries) {
        temp = ml_matches(mt->defs, 0, type, -1, 0); // TODO: use MAX_UNSPECIALIZED_CONFLICTS?
        int guards = 0;
        if (temp == jl_false) {
            cache_with_orig = 1;
        }
        else {
            int unmatched_tvars = 0;
            for (i = 0; i < jl_array_len(temp); i++) {
                jl_value_t *m = jl_array_ptr_ref(temp, i);
                jl_value_t *env = jl_svecref(m, 1);
                int k, l;
                for (k = 0, l = jl_svec_len(env); k < l; k++) {
                    if (jl_is_typevar(jl_svecref(env, k))) {
                        unmatched_tvars = 1;
                        break;
                    }
                }
                if (unmatched_tvars || guards > MAX_UNSPECIALIZED_CONFLICTS) {
                    // if distinguishing a guard entry from the generalized signature
                    // would require matching type vars then bail out, since the
                    // method cache matching algorithm cannot do that.
                    //
                    // also bail if this requires too many guard entries
                    cache_with_orig = 1;
                    break;
                }
                if (((jl_method_t*)jl_svecref(m, 2)) != definition) {
                    guards++;
                }
            }
        }
        if (!cache_with_orig && guards > 0) {
            // use guard entries as placeholders to prevent this cached method
            // from matching when another more specific definition also exists
            size_t i, l;
            guardsigs = jl_alloc_svec(guards);
            temp3 = (jl_value_t*)guardsigs;
            guards = 0;
            for(i = 0, l = jl_array_len(temp); i < l; i++) {
                jl_value_t *m = jl_array_ptr_ref(temp, i);
                if (((jl_method_t*)jl_svecref(m,2)) != definition) {
                    jl_svecset(guardsigs, guards, (jl_tupletype_t*)jl_svecref(m, 0));
                    guards++;
                    //jl_typemap_insert(cache, parent, (jl_tupletype_t*)jl_svecref(m, 0),
                    //        jl_emptysvec, NULL, jl_emptysvec, /*guard*/NULL, jl_cachearg_offset(mt), &lambda_cache, NULL);
                }
            }
        }
    }

    // here we infer types and specialize the method
    newmeth = jl_specializations_get_linfo(definition, type, sparams);

    if (cache_with_orig) {
        // if there is a need to cache with one of the original signatures,
        // the method is still specialized on `types`,
        // but one of the original types will be used as the entry signature
        // in the method cache, possible with a simplesig also,
        // to prevent anything else from matching this entry
        type = origtype; // restore `type` to be the `origtype` backup (discard computed simplified `type`)
        origtype = tt; // choose `tt` as the primary key
        makesimplesig = 0;
    }
    else {
        // don't need `origtype` anymore: `type` is an unambiguous method match
        origtype = type;
    }

    // compute the type this will be cached under
    // if we haven't selected an origtype yet, promote `type`,
    // and then decide if it is beneficial to build a new simplesig
    if (origtype == type) {
        type = NULL; // don't need `type` anymore: it's equivalent to the `origtype`
        if (makesimplesig) {
            // reduce the complexity of rejecting this entry in the cache
            // by replacing non-simple types with jl_any_type to build a new `type`
            // (the only case this applies to currently due to the above logic is jl_function_type)
            size_t np = jl_nparams(origtype);
            newparams = jl_svec_copy(origtype->parameters);
            for (i = 0; i < np; i++) {
                jl_value_t *elt = jl_svecref(newparams, i);
                if (elt == (jl_value_t*)jl_function_type)
                    jl_svecset(newparams, i, jl_any_type);
            }
            type = jl_apply_tuple_type(newparams);
            temp2 = (jl_value_t*)type;
        }
    }

    jl_typemap_insert(cache, parent, origtype, jl_emptysvec, type, guardsigs, (jl_value_t*)newmeth, jl_cachearg_offset(mt), &lambda_cache, NULL);

    if (!newmeth->inferred && !newmeth->inInference) {
        if (jl_options.compile_enabled != JL_OPTIONS_COMPILE_OFF) // don't bother with typeinf if compile is off
            if (jl_symbol_name(definition->name)[0] != '@')  // don't bother with typeinf on macros
                jl_type_infer(newmeth, 0);
    }
    JL_UNLOCK(&codegen_lock); // Might GC
    if (definition->traced && jl_method_tracer)
        jl_call_tracer(jl_method_tracer, (jl_value_t*)newmeth);
    JL_GC_POP();
    return newmeth;
}

static jl_lambda_info_t *jl_mt_assoc_by_type(jl_methtable_t *mt, jl_datatype_t *tt, int cache, int inexact)
{
    jl_typemap_entry_t *entry = NULL;
    jl_svec_t *env = jl_emptysvec;
    jl_method_t *func = NULL;
    jl_tupletype_t *sig = NULL;
    JL_GC_PUSH4(&env, &entry, &func, &sig);

    entry = jl_typemap_assoc_by_type(mt->defs, tt, &env, inexact, 1, 0);
    if (entry == NULL || entry == INEXACT_ENTRY) {
        JL_GC_POP();
        return NULL;
    }

    jl_method_t *m = entry->func.method;
    if (jl_has_call_ambiguities(tt, m)) {
        JL_GC_POP();
        return NULL;
    }
    sig = join_tsig(tt, entry->sig);
    jl_lambda_info_t *nf;
    if (!cache)
        nf = jl_get_specialized(m, sig, env);
    else
        nf = cache_method(mt, &mt->cache, (jl_value_t*)mt, sig, tt, entry, env);
    JL_GC_POP();
    return nf;
}

void print_func_loc(JL_STREAM *s, jl_method_t *m)
{
    long lno = m->line;
    if (lno > 0) {
        char *fname = jl_symbol_name((jl_sym_t*)m->file);
        jl_printf(s, " at %s:%ld", fname, lno);
    }
}

/*
  record ambiguous method priorities

  the relative priority of A and B is ambiguous if
  !subtype(A,B) && !subtype(B,A) && no corresponding tuple
  elements are disjoint.

  for example, (AbstractArray, AbstractMatrix) and (AbstractMatrix, AbstractArray) are ambiguous.
  however, (AbstractArray, AbstractMatrix, Foo) and (AbstractMatrix, AbstractArray, Bar) are fine
  since Foo and Bar are disjoint, so there would be no confusion over
  which one to call.

  There is also this kind of ambiguity: foo{T,S}(T, S) vs. foo(Any,Any)
  In this case jl_types_equal() is true, but one is jl_type_morespecific
  or jl_type_match_morespecific than the other.
  To check this, jl_types_equal_generic needs to be more sophisticated
  so (T,T) is not equivalent to (Any,Any). (TODO)
*/
struct ambiguous_matches_env {
    struct typemap_intersection_env match;
    union jl_typemap_t defs;
    jl_typemap_entry_t *newentry;
    jl_array_t *shadowed;
    int after;
};
const int eager_ambiguity_printing = 0;
static int check_ambiguous_visitor(jl_typemap_entry_t *oldentry, struct typemap_intersection_env *closure0)
{
    struct ambiguous_matches_env *closure = container_of(closure0, struct ambiguous_matches_env, match);
    if (oldentry == closure->newentry) {
        closure->after = 1;
        return 1;
    }
    union jl_typemap_t map = closure->defs;
    jl_tupletype_t *type = (jl_tupletype_t*)closure->match.type;
    jl_method_t *m = closure->newentry->func.method;
    jl_tupletype_t *sig = oldentry->sig;
    jl_value_t *isect = closure->match.ti;
    if (sigs_eq(isect, (jl_value_t*)(closure->after ? sig : type), 1)) {
        // we're ok if the new definition is actually the one we just
        // inferred to be required (see issue #3609). ideally this would
        // never happen, since if New ⊓ Old == New then we should have
        // considered New more specific, but jl_args_morespecific is not
        // perfect, so this is a useful fallback.
        return 1;
    }

    // we know type ∩ sig != Union{} and
    // we know !jl_args_morespecific(type, sig) [before]
    //      or !jl_args_morespecific(sig, type) [after]
    // now we are checking that the reverse is true
    if (!jl_args_morespecific((jl_value_t*)(closure->after ? type : sig),
                              (jl_value_t*)(closure->after ? sig : type))) {
        jl_typemap_entry_t *l = jl_typemap_assoc_by_type(map, (jl_tupletype_t*)isect, NULL, 0, 0, 0);
        if (l != NULL) // ok, intersection is covered
            return 1;
        jl_method_t *mambig = oldentry->func.method;
        if (m->ambig == jl_nothing) {
            m->ambig = (jl_value_t*) jl_alloc_vec_any(0);
            jl_gc_wb(m, m->ambig);
        }
        if (mambig->ambig == jl_nothing) {
            mambig->ambig = (jl_value_t*) jl_alloc_vec_any(0);
            jl_gc_wb(mambig, mambig->ambig);
        }
        jl_array_ptr_1d_push((jl_array_t*) m->ambig, (jl_value_t*) mambig);
        jl_array_ptr_1d_push((jl_array_t*) mambig->ambig, (jl_value_t*) m);
        if (eager_ambiguity_printing) {
            JL_STREAM *s = JL_STDERR;
            jl_printf(s, "WARNING: New definition \n    ");
            jl_static_show_func_sig(s, (jl_value_t*)type);
            print_func_loc(s, m);
            jl_printf(s, "\nis ambiguous with: \n    ");
            jl_static_show_func_sig(s, (jl_value_t*)sig);
            print_func_loc(s, oldentry->func.method);
            jl_printf(s, ".\nTo fix, define \n    ");
            jl_static_show_func_sig(s, isect);
            jl_printf(s, "\nbefore the new definition.\n");
        }
        return 1;  // there may be multiple ambiguities, keep going
    }
    else if (closure->after) {
        // record that this method definition is being partially replaced
        if (closure->shadowed == NULL) {
            closure->shadowed = jl_alloc_vec_any(0);
        }
        jl_array_ptr_1d_push(closure->shadowed, oldentry->func.value);
    }
    return 1;
}

static jl_array_t *check_ambiguous_matches(union jl_typemap_t defs,
                                           jl_typemap_entry_t *newentry)
{
    jl_tupletype_t *type = newentry->sig;
    size_t l = jl_svec_len(type->parameters);
    jl_value_t *va = NULL;
    if (l > 0) {
        va = jl_tparam(type, l - 1);
        if (jl_is_vararg_type(va))
            va = jl_tparam0(va);
        else
            va = NULL;
    }
    struct ambiguous_matches_env env;
    env.match.fptr = check_ambiguous_visitor;
    env.match.type = (jl_value_t*)type;
    env.match.va = va;
    env.match.ti = NULL;
    env.match.env = NULL;
    env.defs = defs;
    env.newentry = newentry;
    env.shadowed = NULL;
    env.after = 0;
    JL_GC_PUSH3(&env.match.env, &env.match.ti, &env.shadowed);
    jl_typemap_intersection_visitor(defs, 0, &env.match);
    JL_GC_POP();
    return env.shadowed;
}

static void method_overwrite(jl_typemap_entry_t *newentry, jl_method_t *oldvalue) {
    // method overwritten
    jl_method_t *method = (jl_method_t*)newentry->func.method;
    jl_module_t *newmod = method->module;
    jl_module_t *oldmod = oldvalue->module;
    JL_STREAM *s = JL_STDERR;
    jl_printf(s, "WARNING: Method definition ");
    jl_static_show_func_sig(s, (jl_value_t*)newentry->sig);
    jl_printf(s, " in module %s", jl_symbol_name(oldmod->name));
    print_func_loc(s, oldvalue);
    jl_printf(s, " overwritten");
    if (oldmod != newmod)
        jl_printf(s, " in module %s", jl_symbol_name(newmod->name));
    print_func_loc(s, method);
    jl_printf(s, ".\n");
}

// invalidate cached methods that overlap this definition
static void invalidate_conflicting(union jl_typemap_t *pml, jl_value_t *type, jl_value_t *parent, jl_array_t *shadowed)
{
    jl_typemap_entry_t **pl;
    if (jl_typeof(pml->unknown) == (jl_value_t*)jl_typemap_level_type) {
        jl_typemap_level_t *cache = pml->node;
        if (cache->arg1 != (void*)jl_nothing) {
            for(int i=0; i < jl_array_len(cache->arg1); i++) {
                union jl_typemap_t *pl = &((union jl_typemap_t*)jl_array_data(cache->arg1))[i];
                if (pl->unknown && pl->unknown != jl_nothing) {
                    invalidate_conflicting(pl, type, (jl_value_t*)cache->arg1, shadowed);
                }
            }
        }
        if (cache->targ != (void*)jl_nothing) {
            for(int i=0; i < jl_array_len(cache->targ); i++) {
                union jl_typemap_t *pl = &((union jl_typemap_t*)jl_array_data(cache->targ))[i];
                if (pl->unknown && pl->unknown != jl_nothing) {
                    invalidate_conflicting(pl, type, (jl_value_t*)cache->targ, shadowed);
                }
            }
        }
        pl = &cache->linear;
        parent = (jl_value_t*)cache;
    }
    else {
        pl = &pml->leaf;
    }
    jl_typemap_entry_t *l = *pl;
    size_t i, n = jl_array_len(shadowed);
    jl_value_t **d = jl_array_ptr_data(shadowed);
    while (l != (void*)jl_nothing) {
        int replaced = 0;
        for (i = 0; i < n; i++) {
            if (d[i] == (jl_value_t*)l->func.linfo->def) {
                replaced = jl_type_intersection(type, (jl_value_t*)l->sig) != (jl_value_t*)jl_bottom_type;
                break;
            }
        }
        if (replaced) {
            *pl = l->next;
            jl_gc_wb(parent, *pl);
        }
        else {
            pl = &l->next;
            parent = (jl_value_t*)l;
        }
        l = l->next;
    }
}

static void update_max_args(jl_methtable_t *mt, jl_tupletype_t *type)
{
    size_t na = jl_nparams(type);
    if (jl_va_tuple_kind(type) == JL_VARARG_UNBOUND)
        na--;
    if (na > mt->max_args)
        mt->max_args = na;
}

void jl_method_table_insert(jl_methtable_t *mt, jl_method_t *method, jl_tupletype_t *simpletype)
{
    assert(jl_is_method(method));
    assert(jl_is_mtable(mt));
    jl_tupletype_t *type = method->sig;
    jl_svec_t *tvars = method->tvars;
    assert(jl_is_tuple_type(type));
    JL_SIGATOMIC_BEGIN();
    jl_value_t *oldvalue = NULL;
    JL_GC_PUSH1(&oldvalue);
    jl_typemap_entry_t *newentry = jl_typemap_insert(&mt->defs, (jl_value_t*)mt,
            type, tvars, simpletype, jl_emptysvec, (jl_value_t*)method, 0, &method_defs, &oldvalue);
    if (oldvalue) {
        method->ambig = ((jl_method_t*)oldvalue)->ambig;
        method_overwrite(newentry, (jl_method_t*)oldvalue);
        jl_array_t *shadowed = jl_alloc_vec_any(1);
        jl_array_ptr_set(shadowed, 0, oldvalue);
        oldvalue = (jl_value_t*)shadowed;
    }
    else {
        oldvalue = (jl_value_t*)check_ambiguous_matches(mt->defs, newentry);
    }
    if (oldvalue)
        invalidate_conflicting(&mt->cache, (jl_value_t*)type, (jl_value_t*)mt, (jl_array_t*)oldvalue);
    JL_GC_POP();
    update_max_args(mt, type);
    JL_SIGATOMIC_END();
}

void JL_NORETURN jl_method_error_bare(jl_function_t *f, jl_value_t *args)
{
    jl_value_t *fargs[3] = {
        (jl_value_t*)jl_methoderror_type,
        (jl_value_t*)f,
        args
    };
    if (fargs[0]) {
        jl_throw(jl_apply_generic(fargs, 3));
    }
    else {
        jl_printf((JL_STREAM*)STDERR_FILENO, "A method error occurred before the base MethodError type was defined. Aborting...\n");
        jl_static_show((JL_STREAM*)STDERR_FILENO,(jl_value_t*)f); jl_printf((JL_STREAM*)STDERR_FILENO,"\n");
        jl_static_show((JL_STREAM*)STDERR_FILENO,args); jl_printf((JL_STREAM*)STDERR_FILENO,"\n");
        jl_bt_size = rec_backtrace(jl_bt_data, JL_MAX_BT_SIZE);
        jl_critical_error(0, NULL, jl_bt_data, &jl_bt_size);
        abort();
    }
    // not reached
}

void JL_NORETURN jl_method_error(jl_function_t *f, jl_value_t **args, size_t na)
{
    jl_value_t *argtup = jl_f_tuple(NULL, args+1, na-1);
    JL_GC_PUSH1(&argtup);
    jl_method_error_bare(f, argtup);
    // not reached
}

jl_datatype_t *jl_wrap_Type(jl_value_t *t);

jl_tupletype_t *arg_type_tuple(jl_value_t **args, size_t nargs)
{
    jl_tupletype_t *tt;
    size_t i;
    if (nargs < jl_page_size/sizeof(jl_value_t*)) {
        jl_value_t **types;
        JL_GC_PUSHARGS(types, nargs);
        for(i=0; i < nargs; i++) {
            jl_value_t *ai = args[i];
            if (jl_is_type(ai))
                types[i] = (jl_value_t*)jl_wrap_Type(ai);
            else
                types[i] = jl_typeof(ai);
        }
        tt = (jl_tupletype_t*)jl_inst_concrete_tupletype_v(types, nargs);
        JL_GC_POP();
    }
    else {
        jl_svec_t *types = jl_alloc_svec(nargs);
        JL_GC_PUSH1(&types);
        for(i=0; i < nargs; i++) {
            jl_value_t *ai = args[i];
            if (jl_is_type(ai))
                jl_svecset(types, i, (jl_value_t*)jl_wrap_Type(ai));
            else
                jl_svecset(types, i, jl_typeof(ai));
        }
        tt = (jl_tupletype_t*)jl_inst_concrete_tupletype(types);
        JL_GC_POP();
    }
    return tt;
}

jl_lambda_info_t *jl_method_lookup_by_type(jl_methtable_t *mt, jl_tupletype_t *types,
                                           int cache, int inexact)
{
    jl_typemap_entry_t *entry = jl_typemap_assoc_by_type(mt->cache, types, NULL, 0, 1, jl_cachearg_offset(mt));
    jl_lambda_info_t *sf;
    if (entry) {
        sf = entry->func.linfo;
    }
    else {
        if (jl_is_leaf_type((jl_value_t*)types)) cache=1;
        sf = jl_mt_assoc_by_type(mt, types, cache, inexact);
    }
    return sf;
}

JL_DLLEXPORT int jl_method_exists(jl_methtable_t *mt, jl_tupletype_t *types)
{
    return jl_method_lookup_by_type(mt, types, 0, 0) != NULL;
}

jl_lambda_info_t *jl_method_lookup(jl_methtable_t *mt, jl_value_t **args, size_t nargs, int cache)
{
    jl_lambda_info_t *sf;
    jl_typemap_entry_t *entry = jl_typemap_assoc_exact(mt->cache, args, nargs, jl_cachearg_offset(mt));
    if (entry == NULL) {
        jl_tupletype_t *tt = arg_type_tuple(args, nargs);
        JL_GC_PUSH1(&tt);
        sf = jl_mt_assoc_by_type(mt, tt, cache, 0);
        JL_GC_POP();
    }
    else {
        sf = entry->func.linfo;
    }

    return sf;
}

JL_DLLEXPORT jl_value_t *jl_matching_methods(jl_tupletype_t *types, int lim, int include_ambiguous);

// compile-time method lookup
jl_lambda_info_t *jl_get_specialization1(jl_tupletype_t *types)
{
    assert(jl_nparams(types) > 0);
    if (!jl_is_leaf_type((jl_value_t*)types) || jl_has_typevars((jl_value_t*)types))
        return NULL;
    assert(jl_is_datatype(jl_tparam0(types)));

    // make sure exactly 1 method matches (issue #7302).
    int i;
    for(i=0; i < jl_nparams(types); i++) {
        jl_value_t *ti = jl_tparam(types, i);
        // if one argument type is DataType, multiple Type{} definitions
        // might match. also be conservative with tuples rather than trying
        // to analyze them in detail.
        if (ti == (jl_value_t*)jl_datatype_type || jl_is_tuple_type(ti)) {
            jl_value_t *matches = jl_matching_methods(types, 1, 0);
            if (matches == jl_false)
                return NULL;
            break;
        }
    }

    jl_methtable_t *mt = ((jl_datatype_t*)jl_tparam0(types))->name->mt;
    jl_lambda_info_t *sf = NULL;
    // most of the time sf is rooted in mt, but if the method is staged it may
    // not be the case
    JL_GC_PUSH1(&sf);
    JL_TRY {
        sf = jl_method_lookup_by_type(mt, types, 1, 1);
    } JL_CATCH {
        goto not_found;
    }
    if (sf != NULL) {
        jl_method_t *m = sf->def;
        if (jl_has_call_ambiguities(types, m)) {
            goto not_found;
        }
    }
    if (sf == NULL || sf->code == NULL || sf->inInference)
        goto not_found;
    if (sf->functionObjectsDecls.functionObject == NULL) {
        if (sf->fptr != NULL)
            goto not_found;
        if (sf->code == jl_nothing) {
            jl_type_infer(sf, 0);
            if (sf->code == jl_nothing)
                goto not_found;
        }
        jl_compile_linfo(sf);
    }
    JL_GC_POP();
    return sf;
 not_found:
    JL_GC_POP();
    return NULL;
}

JL_DLLEXPORT int jl_compile_hint(jl_tupletype_t *types)
{
    return jl_get_specialization1(types) != NULL;
}

int jl_has_call_ambiguities(jl_tupletype_t *types, jl_method_t *m)
{
    if (m->ambig == jl_nothing) return 0;
    for (size_t i = 0; i < jl_array_len(m->ambig); i++) {
        jl_method_t *mambig = (jl_method_t*)jl_array_ptr_ref(m->ambig, i);
        if (jl_type_intersection((jl_value_t*)mambig->sig,
                                 (jl_value_t*)types) != (jl_value_t*)jl_bottom_type) {
            return 1;
        }
    }
    return 0;
}

// add type of `f` to front of argument tuple type
jl_tupletype_t *jl_argtype_with_function(jl_function_t *f, jl_tupletype_t *types)
{
    size_t l = jl_nparams(types);
    jl_value_t *tt = (jl_value_t*)jl_alloc_svec(1+l);
    size_t i;
    JL_GC_PUSH1(&tt);
    if (jl_is_type(f))
        jl_svecset(tt, 0, jl_wrap_Type(f));
    else
        jl_svecset(tt, 0, jl_typeof(f));
    for(i=0; i < l; i++)
        jl_svecset(tt, i+1, jl_tparam(types,i));
    tt = (jl_value_t*)jl_apply_tuple_type((jl_svec_t*)tt);
    JL_GC_POP();
    return (jl_tupletype_t*)tt;
}

static int tupletype_any_bottom(jl_value_t *sig)
{
    jl_svec_t *types = ((jl_tupletype_t*)sig)->types;
    size_t i, l = jl_svec_len(types);
    for (i = 0; i < l; i++) {
        if (jl_svecref(types, i) == jl_bottom_type)
            return 1;
    }
    return 0;
}

static int _compile_all_tvar_union(jl_tupletype_t *methsig, jl_svec_t *tvars)
{
    // f{<:Union{...}}(...) is a common pattern
    // and expanding the Union may give a leaf function
    jl_tvar_t **tvs;
    int tvarslen;
    if (jl_is_typevar(tvars)) {
        tvs = (jl_tvar_t**)&tvars;
        tvarslen = 1;
    }
    else {
        tvs = (jl_tvar_t**)jl_svec_data(tvars);
        tvarslen = jl_svec_len(tvars);
        if (tvarslen == 0) {
            if (jl_is_leaf_type((jl_value_t*)methsig)) {
                // usually can create a specialized version of the function,
                // if the signature is already a leaftype
                jl_lambda_info_t *spec = jl_get_specialization1(methsig);
                if (spec)
                    return 1;
            }
            return 0;
        }
    }

    int complete = 1;
    jl_value_t **env;
    JL_GC_PUSHARGS(env, 2 * tvarslen);
    int *idx = (int*)alloca(sizeof(int) * tvarslen);
    int i;
    for (i = 0; i < tvarslen; i++) {
        idx[i] = 0;
        env[2 * i] = (jl_value_t*)tvs[i];
        env[2 * i + 1] = jl_bottom_type; // initialize the list with Union{}, since T<:Union{} is always a valid option
    }

    for (i = 0; i < tvarslen; /* incremented by inner loop */) {
        jl_value_t *sig;
        JL_TRY {
            sig = (jl_value_t*)
                jl_instantiate_type_with((jl_value_t*)methsig, env, tvarslen);
        }
        JL_CATCH {
            goto getnext; // sigh, we found an invalid type signature. should we warn the user?
        }
        assert(jl_is_tuple_type(sig));
        if (sig == jl_bottom_type || tupletype_any_bottom(sig)) {
            goto getnext; // signature wouldn't be callable / is invalid -- skip it
        }
        if (jl_is_leaf_type(sig)) {
            if (jl_get_specialization1((jl_tupletype_t*)sig)) {
                if (!jl_has_typevars((jl_value_t*)sig)) goto getnext; // success
            }
        }
        complete = 0;

getnext:
        for (i = 0; i < tvarslen; i++) {
            jl_tvar_t *tv = tvs[i];
            if (jl_is_uniontype(tv->ub)) {
                jl_uniontype_t *ub = (jl_uniontype_t*)tv->ub;
                size_t l = jl_svec_len(ub->types);
                size_t j = idx[i];
                if (j == l) {
                    env[2 * i + 1] = jl_bottom_type;
                    idx[i] = 0;
                }
                else {
                    jl_value_t *ty = jl_svecref(ub->types, j);
                    if (!jl_is_leaf_type(ty))
                        ty = (jl_value_t*)jl_new_typevar(tv->name, tv->lb, ty);
                    env[2 * i + 1] = ty;
                    idx[i] = j + 1;
                    break;
                }
            }
            else {
                env[2 * i + 1] = (jl_value_t*)tv;
                complete = 0;
            }
        }
    }
    JL_GC_POP();
    return complete;
}

static int _compile_all_union(jl_tupletype_t *sig, jl_svec_t *tvars)
{
    // f(::Union{...}, ...) is a common pattern
    // and expanding the Union may give a leaf function
    int complete = 1;
    size_t count_unions = 0;
    size_t i, l = jl_svec_len(sig->parameters);
    jl_svec_t *p = NULL;
    jl_tupletype_t *methsig = NULL;

    for (i = 0; i < l; i++) {
        jl_value_t *ty = jl_svecref(sig->parameters, i);
        if (jl_is_uniontype(ty)) {
            jl_svec_t *utypes = ((jl_uniontype_t*)ty)->types;
            size_t l = jl_svec_len(utypes);
            if (l == 0)
                return 1; // why does this method exist?
            ++count_unions;
        }
    }

    if (count_unions == 0)
        return _compile_all_tvar_union(sig, tvars);

    int *idx = (int*)alloca(sizeof(int) * count_unions);
    for (i = 0; i < count_unions; i++) {
        idx[i] = 0;
    }

    JL_GC_PUSH2(&p, &methsig);
    int idx_ctr = 0, incr = 0;
    while (!incr) {
        jl_svec_t *p = jl_alloc_svec_uninit(l);
        for (i = 0, idx_ctr = 0, incr = 1; i < l; i++) {
            jl_value_t *ty = jl_svecref(sig->parameters, i);
            if (jl_is_uniontype(ty)) {
                jl_svec_t *utypes = ((jl_uniontype_t*)ty)->types;
                size_t l = jl_svec_len(utypes);
                size_t j = idx[idx_ctr];
                jl_svecset(p, i, jl_svecref(utypes, j));
                ++j;
                if (incr) {
                    if (j == l) {
                        idx[idx_ctr] = 0;
                    }
                    else {
                        idx[idx_ctr] = j;
                        incr = 0;
                    }
                }
                ++idx_ctr;
            }
            else {
                jl_svecset(p, i, ty);
            }
        }
        methsig = jl_apply_tuple_type(p);
        if (!_compile_all_tvar_union(methsig, tvars))
            complete = 0;
    }

    JL_GC_POP();
    return complete;
}

static void _compile_all_deq(jl_array_t *found)
{
    int found_i, found_l = jl_array_len(found);
    jl_printf(JL_STDERR, "found %d uncompiled methods for compile-all\n", (int)found_l);
    jl_lambda_info_t *linfo = NULL;
    JL_GC_PUSH1(&linfo);
    for (found_i = 0; found_i < found_l; found_i++) {
        if (found_i % (found_l / 300) == 0 || found_i == found_l - 1) // show 300 progress steps, to show progress without overwhelming log files
            jl_printf(JL_STDERR, " %d / %d\r", found_i + 1, found_l);
        jl_typemap_entry_t *ml = (jl_typemap_entry_t*)jl_array_ptr_ref(found, found_i);
        if (ml->func.value == NULL)
            continue; // XXX: how does this happen

        jl_lambda_info_t *templ = NULL;
        if (jl_is_method(ml->func.value)) {
            // type infer a copy of the template, to avoid modifying the template itself
            templ = ml->func.method->lambda_template;
            if (templ->unspecialized_ducttape)
                linfo = templ->unspecialized_ducttape; // TODO: switch to using the ->tfunc field to store/retrieve this
            else
                linfo = jl_get_specialized(ml->func.method, ml->sig, jl_emptysvec);
        }
        else if (jl_is_lambda_info(ml->func.value)) {
            templ = ml->func.linfo;
            linfo = ml->func.linfo;
        }
        else {
            continue; // this should be unreachable
        }

        if (!linfo->inferred) {
            // force this function to be recompiled
            jl_type_infer(linfo, 1);
            linfo->functionObjectsDecls.functionObject = NULL;
            linfo->functionObjectsDecls.specFunctionObject = NULL;
            linfo->functionID = 0;
            linfo->specFunctionID = 0;
            linfo->jlcall_api = 0;
        }

        // keep track of whether all possible signatures have been cached (and thus whether it can skip trying to compile the template function)
        // this is necessary because many intrinsics try to call static_eval and thus are not compilable unspecialized
        int complete = _compile_all_union(ml->sig, ml->tvars);
        if (complete) {
            if (!templ->functionID)
                // indicate that this method doesn't need a functionID because it was fully covered above
                templ->functionID = -1;
        }
        else {
            jl_compile_linfo(linfo);
            assert(linfo->functionID > 0);
            if (linfo != templ) {
                // copy the function pointer back to the template
                templ->functionObjectsDecls.functionObject = linfo->functionObjectsDecls.functionObject;
                templ->functionObjectsDecls.specFunctionObject = linfo->functionObjectsDecls.specFunctionObject;
                templ->functionID = linfo->functionID;
                templ->specFunctionID = linfo->specFunctionID;
                templ->jlcall_api = linfo->jlcall_api;
                templ->unspecialized_ducttape = linfo;
                jl_gc_wb(templ, linfo);
            }
        }
    }
    JL_GC_POP();
    jl_printf(JL_STDERR, "\n");
}

static int _compile_all_enq(jl_typemap_entry_t *ml, void *env)
{
    jl_array_t *found = (jl_array_t*)env;
    jl_lambda_info_t *linfo = NULL;
    if (ml->func.value == NULL)
        return 1;

    if (jl_is_method(ml->func.value)) {
        // method definition -- compile via template field
        jl_method_t *m = ml->func.method;
        if (m->invokes.unknown != NULL)
            jl_typemap_visitor(m->invokes, _compile_all_enq, env);
        if (!m->isstaged)
            linfo = m->lambda_template;
    }
    else if (jl_is_lambda_info(ml->func.value)) {
        linfo = ml->func.linfo;
        if (linfo->fptr != NULL)
            return 1; // builtin function
    }
    if (linfo && !linfo->functionID) {
        // found a lambda that still needs to be compiled
        jl_array_ptr_1d_push(found, (jl_value_t*)ml);
    }
    return 1;
}

static void _compile_all_enq_module(jl_module_t *m, jl_array_t *found)
{
    // scan through all types reachable from 'v' and
    // record all jl_lambda_info_t objects and signatures in their method tables
    size_t i, sz = m->bindings.size;
    for(i=1; i < sz; i+=2) {
        if (m->bindings.table[i] != HT_NOTFOUND) {
            jl_binding_t *b = (jl_binding_t*)m->bindings.table[i];
            if (b->owner == m && b->value && b->constp) {
                jl_value_t *v = b->value;
                if (jl_is_datatype(v)) {
                    jl_typename_t *tn = ((jl_datatype_t*)v)->name;
                    if (tn->module == m && tn->name == b->name) {
                        jl_methtable_t *mt = tn->mt;
                        if (mt != NULL && (jl_value_t*)mt != jl_nothing) {
                            jl_typemap_visitor(mt->defs, _compile_all_enq, (void*)found);
                            jl_typemap_visitor(mt->cache, _compile_all_enq, (void*)found);
                        }
                    }
                }
                else if (jl_is_module(v)) {
                    jl_module_t *child = (jl_module_t*)b->value;
                    if (child != m && child->parent == m && child->name == b->name) {
                        // this is the original/primary binding for the submodule
                        _compile_all_enq_module(child, found);
                    }
                }
            }
        }
    }
}

static void jl_compile_all(void)
{
    // this "found" array will contain
    // TypeMapEntries for Methods and LambdaInfos that need to be compiled
    jl_array_t *m = jl_alloc_vec_any(0);
    JL_GC_PUSH1(&m);
    while (1) {
        _compile_all_enq_module(jl_main_module, m);
        size_t changes = jl_array_len(m);
        if (!changes)
            break;
        _compile_all_deq(m);
        jl_array_del_end(m, changes);
    }
    JL_GC_POP();
}

static int _precompile_enq_tfunc(jl_typemap_entry_t *l, void *closure)
{
    if (jl_is_lambda_info(l->func.value) && !l->func.linfo->functionID)
        jl_array_ptr_1d_push((jl_array_t*)closure, (jl_value_t*)l->func.linfo->specTypes);
    return 1;
}

static int _precompile_enq_spec(jl_typemap_entry_t *def, void *closure)
{
    jl_typemap_visitor(def->func.method->specializations, _precompile_enq_tfunc, closure);
    return 1;
}

static void _precompile_enq_module(jl_module_t *m, jl_array_t *unspec)
{
    // removes all method caches
    size_t i;
    void **table = m->bindings.table;
    for(i=1; i < m->bindings.size; i+=2) {
        if (table[i] != HT_NOTFOUND) {
            jl_binding_t *b = (jl_binding_t*)table[i];
            if (b->owner == m && b->value && b->constp) {
                if (jl_is_datatype(b->value)) {
                    jl_typename_t *tn = ((jl_datatype_t*)b->value)->name;
                    if (tn->module == m && tn->name == b->name) {
                        jl_methtable_t *mt = tn->mt;
                        if (mt != NULL && (jl_value_t*)mt != jl_nothing) {
                            jl_typemap_visitor(mt->defs, _precompile_enq_spec, (void*)unspec);
                        }
                    }
                }
                else if (jl_is_module(b->value)) {
                    jl_module_t *child = (jl_module_t*)b->value;
                    if (child != m && child->parent == m && child->name == b->name) {
                        // this is the original/primary binding for the submodule
                        _precompile_enq_module((jl_module_t*)b->value, unspec);
                    }
                }
            }
        }
    }
}

static void jl_compile_specializations(void)
{
    // this "found" array will contain function
    // type signatures that were inferred but haven't been compiled
    jl_array_t *m = jl_alloc_vec_any(0);
    JL_GC_PUSH1(&m);
    _precompile_enq_module(jl_main_module, m);
    size_t i, l;
    for (i = 0, l = jl_array_len(m); i < l; i++) {
        jl_compile_hint((jl_tupletype_t*)jl_array_ptr_ref(m, i));
    }
    JL_GC_POP();
}

void jl_precompile(int all) {
    jl_compile_specializations();
    if (all)
        jl_compile_all();
}

//

#ifdef JL_TRACE
static int trace_en = 0;
static int error_en = 1;
static void __attribute__ ((unused)) enable_trace(int x) { trace_en=x; }
static void show_call(jl_value_t *F, jl_value_t **args, uint32_t nargs)
{
    jl_printf(JL_STDOUT, "%s(",  jl_symbol_name(jl_gf_name(F)));
    for(size_t i=0; i < nargs; i++) {
        if (i > 0) jl_printf(JL_STDOUT, ", ");
        jl_static_show(JL_STDOUT, jl_typeof(args[i]));
    }
    jl_printf(JL_STDOUT, ")");
}
#endif

static jl_value_t *verify_type(jl_value_t *v)
{
    assert(jl_typeof(jl_typeof(v)));
    return v;
}

STATIC_INLINE uint32_t int32hash_fast(uint32_t a)
{
//    a = (a+0x7ed55d16) + (a<<12);
//    a = (a^0xc761c23c) ^ (a>>19);
//    a = (a+0x165667b1) + (a<<5);
//    a = (a+0xd3a2646c) ^ (a<<9);
//    a = (a+0xfd7046c5) + (a<<3);
//    a = (a^0xb55a4f09) ^ (a>>16);
    return a;
}

STATIC_INLINE int sig_match_fast(jl_value_t **args, jl_value_t **sig, size_t i, size_t n)
{
    // NOTE: This function is a huge performance hot spot!!
    for (; i < n; i++) {
        jl_value_t *decl = sig[i];
        jl_value_t *a = args[i];
        if ((jl_value_t*)jl_typeof(a) != decl) {
            /*
              we are only matching concrete types here, and those types are
              hash-consed, so pointer comparison should work.
            */
            return 0;
        }
    }
    return 1;
}

//STATIC_INLINE jl_typemap_entry_t *sig2_match_fast(jl_value_t **args, jl_typemap_entry_t *entry1, jl_typemap_entry_t *entry2, size_t n)
//{
//    // NOTE: This function is a huge performance hot spot!!
//    jl_value_t **sig1 = jl_svec_data(entry1->sig->parameters);
//    jl_value_t **sig2 = jl_svec_data(entry2->sig->parameters);
//    size_t i;
//    for (i = 0; i < n; i++) {
//        jl_value_t *decl1 = sig1[i];
//        jl_value_t *decl2 = sig2[i];
//        jl_value_t *a = args[i];
//        jl_value_t *ta = jl_typeof(a);
//        if (ta != decl1) {
//            /*
//              we are only matching concrete types here, and those types are
//              hash-consed, so pointer comparison should work.
//            */
//            return sig_match_fast(args, sig2, i, n) ? entry2 : NULL;
//        }
//        if (ta != decl2) {
//            /*
//              we are only matching concrete types here, and those types are
//              hash-consed, so pointer comparison should work.
//            */
//            return sig_match_fast(args, sig1, i, n) ? entry1 : NULL;
//        }
//    }
//    return entry1;
//}

jl_typemap_entry_t *call_cache[N_CALL_CACHE];
static uint8_t pick_which[N_CALL_CACHE];
#ifdef JL_GF_PROFILE
void call_cache_stats() {
    int pick_which_stat[4] = {0, 0, 0, 0};
    int i, count = 0;
    for (i = 0; i < N_CALL_CACHE; i++) {
        if (call_cache[i])
            count++;
    }
    for (i = 0; i < N_CALL_CACHE; i++) {
        ++pick_which_stat[pick_which[i] & 3];
    }
    jl_safe_printf("cache occupied: %d / %d; pick_which stats: {%d, %d, %d, %d}\n",
            count, N_CALL_CACHE,
            pick_which_stat[0], pick_which_stat[1], pick_which_stat[2], pick_which_stat[3]);
}
#endif

#ifdef _COMPILER_MICROSOFT_
#define __builtin_return_address(n) _ReturnAddress()
#endif

JL_DLLEXPORT jl_value_t *jl_apply_generic(jl_value_t **args, uint32_t nargs)
{
#ifdef JL_GF_PROFILE
    mt->ncalls++;
#endif
#ifdef JL_TRACE
    int traceen = trace_en; //&& ((char*)&mt < jl_stack_hi-6000000);
    if (traceen)
        show_call(args[0], &args[1], nargs-1);
#endif

    /*
      search order:
      check associative hash based on callsite address for leafsig match
      look at concrete signatures
      if there is an exact match, return it
      otherwise look for a matching generic signature
      if no concrete or generic match, raise error
      if no generic match, use the concrete one even if inexact
      otherwise instantiate the generic method and use it
    */
    uint32_t callsite = int32hash_fast((uintptr_t)__builtin_return_address(0));
    // implementation 1
//    uint32_t cache_idx1 = (callsite) & (N_CALL_CACHE - 1);
//    uint32_t cache_idx2 = (callsite >> 16) & (N_CALL_CACHE - 1);
//    jl_typemap_entry_t *entry1 = call_cache[cache_idx1];
//    jl_typemap_entry_t *entry2 = call_cache[cache_idx2];
//    jl_methtable_t *mt = NULL;
//    jl_typemap_entry_t *entry = NULL;
//    if (entry1 && nargs == jl_svec_len(entry1->sig->parameters)) {
//        if (entry2 && nargs == jl_svec_len(entry2->sig->parameters))
//            entry = sig2_match_fast(args, entry1, entry2, nargs);
//        else if (sig_match_fast(args, jl_svec_data(entry1->sig->parameters), 0, nargs))
//            entry = entry1;
//    }
//    else if (entry2 && nargs == jl_svec_len(entry2->sig->parameters)) {
//        if (sig_match_fast(args, jl_svec_data(entry2->sig->parameters), 0, nargs))
//            entry = entry2;
//    }
//
//    if (entry == NULL) {
//        jl_value_t *F = args[0];
//        mt = jl_gf_mtable(F);
//        entry = jl_typemap_assoc_exact(mt->cache, args, nargs, jl_cachearg_offset(mt));
//        if (entry && entry->isleafsig && entry->simplesig == (void*)jl_nothing && entry->guardsigs == jl_emptysvec) {
//            switch (++pick_which[cache_idx1] & 1) {
//            case 0:
//                call_cache[cache_idx1] = entry;
//                break;
//            case 1:
//                call_cache[cache_idx2] = entry;
//                break;
//            }
//        }
//    }

    // implementation 2
//    uint32_t cache_idx1 = (callsite) & (N_CALL_CACHE - 1);
//    uint32_t cache_idx2 = (callsite >> 8) & (N_CALL_CACHE - 1);
//    uint32_t cache_idx3 = (callsite >> 16) & (N_CALL_CACHE - 1);
//    uint32_t cache_idx4 = (callsite >> 24) & (N_CALL_CACHE - 1);
//    jl_typemap_entry_t *entry = call_cache[cache_idx1];
//    jl_methtable_t *mt = NULL;
//    if (!entry || nargs != jl_svec_len(entry->sig->parameters) ||
//            !sig_match_fast(args, jl_svec_data(entry->sig->parameters), nargs)) {
//        entry = call_cache[cache_idx2];
//        if (!entry || nargs != jl_svec_len(entry->sig->parameters) ||
//                !sig_match_fast(args, jl_svec_data(entry->sig->parameters), nargs)) {
//            entry = call_cache[cache_idx3];
//            if (!entry || nargs != jl_svec_len(entry->sig->parameters) ||
//                    !sig_match_fast(args, jl_svec_data(entry->sig->parameters), nargs)) {
//                entry = call_cache[cache_idx4];
//                if (!entry || nargs != jl_svec_len(entry->sig->parameters) ||
//                    !sig_match_fast(args, jl_svec_data(entry->sig->parameters), nargs)) {
//
//                    jl_value_t *F = args[0];
//                    mt = jl_gf_mtable(F);
//                    entry = jl_typemap_assoc_exact(mt->cache, args, nargs, jl_cachearg_offset(mt));
//                    if (entry && entry->isleafsig && entry->simplesig == (void*)jl_nothing && entry->guardsigs == jl_emptysvec) {
//                        switch (++pick_which[cache_idx1] & 3) {
//                        case 0:
//                            call_cache[cache_idx1] = entry;
//                            break;
//                        case 1:
//                            call_cache[cache_idx2] = entry;
//                            break;
//                        case 2:
//                            call_cache[cache_idx3] = entry;
//                            break;
//                        case 3:
//                            call_cache[cache_idx4] = entry;
//                            break;
//                        }
//                    }
//                }
//            }
//        }
//    }

    // implementation 3
    // compute the entry hashes
    // use different parts of the value
    // so that a collision across all of
    // them is less likely
    uint32_t cache_idx[4] = {
        (callsite) & (N_CALL_CACHE - 1),
        (callsite >> 8) & (N_CALL_CACHE - 1),
        (callsite >> 16) & (N_CALL_CACHE - 1),
        (callsite >> 24 | callsite << 8) & (N_CALL_CACHE - 1)};
    jl_typemap_entry_t *entry = NULL;
    jl_methtable_t *mt = NULL;
    int i;
    // check each cache entry to see if it matches
    for (i = 0; i < 4; i++) {
        entry = call_cache[cache_idx[i]];
        if (entry && nargs == jl_svec_len(entry->sig->parameters) &&
            sig_match_fast(args, jl_svec_data(entry->sig->parameters), 0, nargs)) {
            break;
        }
    }
    // if no method was found in the associative cache, check the full cache
    if (i == 4) {
        jl_value_t *F = args[0];
        mt = jl_gf_mtable(F);
        entry = jl_typemap_assoc_exact(mt->cache, args, nargs, jl_cachearg_offset(mt));
        if (entry && entry->isleafsig && entry->simplesig == (void*)jl_nothing && entry->guardsigs == jl_emptysvec) {
            // put the entry into the cache if it's valid for a leaftype lookup,
            // using pick_which to slightly randomize where it ends up
            call_cache[cache_idx[++pick_which[cache_idx[0]] & 3]] = entry;
        }
    }

    jl_lambda_info_t *mfunc = NULL;
    jl_tupletype_t *tt = NULL;
    if (entry == NULL) {
        // cache miss case
        tt = arg_type_tuple(args, nargs);
        // if running inference overwrites this particular method, it becomes
        // unreachable from the method table, so root mfunc.
        JL_GC_PUSH2(&tt, &mfunc);
        mfunc = jl_mt_assoc_by_type(mt, tt, 1, 0);

        if (mfunc == NULL) {
#ifdef JL_TRACE
            if (error_en)
                show_call(F, args, nargs);
#endif
            JL_GC_POP();
            jl_method_error((jl_function_t*)args[0], args, nargs);
            // unreachable
        }
        jl_value_t *res = jl_call_method_internal(mfunc, args, nargs);
        JL_GC_POP();
        return verify_type(res);
    }

    mfunc = entry->func.linfo;
#ifdef JL_TRACE
    if (traceen)
        jl_printf(JL_STDOUT, " at %s:%d\n", jl_symbol_name(mfunc->file), mfunc->line);
#endif
    jl_value_t *res = jl_call_method_internal(mfunc, args, nargs);
    return verify_type(res);
}

JL_DLLEXPORT jl_value_t *jl_gf_invoke_lookup(jl_datatype_t *types)
{
    jl_methtable_t *mt = ((jl_datatype_t*)jl_tparam0(types))->name->mt;
    jl_typemap_entry_t *entry = jl_typemap_assoc_by_type(mt->defs, types, /*don't record env*/NULL,
            /*exact match*/0, /*subtype*/1, /*offs*/0);
    if (!entry)
        return jl_nothing;
    return (jl_value_t*)entry;
}

// invoke()
// this does method dispatch with a set of types to match other than the
// types of the actual arguments. this means it sometimes does NOT call the
// most specific method for the argument types, so we need different logic.
// first we use the given types to look up a definition, then we perform
// caching and specialization within just that definition.
// every definition has its own private method table for this purpose.
//
// NOTE: assumes argument type is a subtype of the lookup type.
jl_value_t *jl_gf_invoke(jl_tupletype_t *types0, jl_value_t **args, size_t nargs)
{
    jl_svec_t *tpenv = jl_emptysvec;
    jl_tupletype_t *newsig = NULL;
    jl_tupletype_t *tt = NULL;
    jl_tupletype_t *types = NULL;
    jl_tupletype_t *sig = NULL;
    JL_GC_PUSH5(&types, &tpenv, &newsig, &sig, &tt);
    jl_value_t *gf = args[0];
    types = (jl_datatype_t*)jl_argtype_with_function(gf, (jl_tupletype_t*)types0);
    jl_methtable_t *mt = jl_gf_mtable(gf);
    jl_typemap_entry_t *entry = (jl_typemap_entry_t*)jl_gf_invoke_lookup(types);

    if ((jl_value_t*)entry == jl_nothing) {
        jl_method_error_bare(gf, (jl_value_t*)types0);
        // unreachable
    }

    // now we have found the matching definition.
    // next look for or create a specialization of this definition.

    jl_lambda_info_t *mfunc = NULL;
    jl_typemap_entry_t *tm = NULL;
    if (entry->func.method->invokes.unknown != NULL)
        tm = jl_typemap_assoc_exact(entry->func.method->invokes, args, nargs, jl_cachearg_offset(mt));
    if (tm == NULL) {
        tt = arg_type_tuple(args, nargs);
        if (entry->tvars != jl_emptysvec) {
            jl_value_t *ti =
                jl_lookup_match((jl_value_t*)tt, (jl_value_t*)entry->sig, &tpenv, entry->tvars);
            assert(ti != (jl_value_t*)jl_bottom_type);
            (void)ti;
        }
        sig = join_tsig(tt, entry->sig);
        jl_method_t *func = entry->func.method;

        if (func->invokes.unknown == NULL)
            func->invokes.unknown = jl_nothing;

        mfunc = cache_method(mt, &func->invokes, entry->func.value, sig, tt, entry, tpenv);
    }
    else {
        mfunc = tm->func.linfo;
    }
    JL_GC_POP();
    return jl_call_method_internal(mfunc, args, nargs);
}

static jl_function_t *jl_new_generic_function_with_supertype(jl_sym_t *name, jl_module_t *module, jl_datatype_t *st, int iskw)
{
    // type name is function name prefixed with #
    size_t l = strlen(jl_symbol_name(name));
    char *prefixed;
    if (iskw) {
        prefixed = (char*)malloc(l+5);
        strcpy(&prefixed[0], "#kw#");
        strcpy(&prefixed[4], jl_symbol_name(name));
    }
    else {
        prefixed = (char*)malloc(l+2);
        prefixed[0] = '#';
        strcpy(&prefixed[1], jl_symbol_name(name));
    }
    jl_sym_t *tname = jl_symbol(prefixed);
    free(prefixed);
    jl_datatype_t *ftype = jl_new_datatype(tname, st, jl_emptysvec, jl_emptysvec, jl_emptysvec, 0, 0, 0);
    JL_GC_PUSH1(&ftype);
    ftype->name->mt->name = name; jl_gc_wb(ftype->name->mt, name);
    ftype->name->module = module; jl_gc_wb(ftype->name, module);
    ftype->name->mt->module = module; jl_gc_wb(ftype->name->mt, module);
    jl_set_const(module, tname, (jl_value_t*)ftype);
    jl_value_t *f = jl_new_struct(ftype);
    ftype->instance = f; jl_gc_wb(ftype, f);
    JL_GC_POP();
    return (jl_function_t*)f;
}

JL_DLLEXPORT jl_function_t *jl_get_kwsorter(jl_typename_t *tn)
{
    jl_methtable_t *mt = tn->mt;
    if (!mt->kwsorter) {
        mt->kwsorter = jl_new_generic_function_with_supertype(tn->name, mt->module, jl_function_type, 1);
        jl_gc_wb(mt, mt->kwsorter);
    }
    return mt->kwsorter;
}

jl_function_t *jl_new_generic_function(jl_sym_t *name, jl_module_t *module)
{
    return jl_new_generic_function_with_supertype(name, module, jl_function_type, 0);
}

JL_DLLEXPORT jl_svec_t *jl_match_method(jl_value_t *type, jl_value_t *sig,
                                        jl_svec_t *tvars)
{
    jl_svec_t *env = jl_emptysvec;
    jl_value_t *ti=NULL;
    JL_GC_PUSH2(&env, &ti);
    ti = jl_lookup_match(type, (jl_value_t*)sig, &env, tvars);
    jl_svec_t *result = jl_svec2(ti, env);
    JL_GC_POP();
    return result;
}

// Determine whether a typevar exists inside at most one DataType.
// These are the typevars that will always be matched by any matching
// arguments.
static int tvar_exists_at_top_level(jl_value_t *tv, jl_tupletype_t *sig, int attop)
{
    int i, l=jl_nparams(sig);
    for(i=0; i < l; i++) {
        jl_value_t *a = jl_tparam(sig, i);
        if (jl_is_vararg_type(a))
            a = jl_tparam0(a);
        if (a == tv)
            return 1;
        if (attop && jl_is_datatype(a)) {
            jl_svec_t *p = ((jl_datatype_t*)a)->parameters;
            int j;
            for(j=0; j < jl_svec_len(p); j++) {
                if (jl_svecref(p,j) == tv)
                    return 1;
            }
        }
    }
    return 0;
}

struct ml_matches_env {
    struct typemap_intersection_env match;
    jl_value_t *t;     // results: array of svec(argtypes, params, Method)
    jl_svec_t *matc;   // current working svec
    int lim;
    int include_ambiguous;  // whether ambiguous matches should be included
};
static int ml_matches_visitor(jl_typemap_entry_t *ml, struct typemap_intersection_env *closure0)
{
    struct ml_matches_env *closure = container_of(closure0, struct ml_matches_env, match);
    int i;
    // a method is shadowed if type <: S <: m->sig where S is the
    // signature of another applicable method
    /*
      more generally, we can stop when the type is a subtype of the
      union of all the signatures examined so far.
    */
    jl_method_t *meth = ml->func.method;
    assert(meth);
    int skip = 0;
    size_t len = jl_array_len(closure->t);
    if (closure->lim >= 0) {
        // we can skip this match if the types are already covered
        // by a prior (more specific) match. but only do this in
        // the "limited" mode used by type inference.
        for (i = 0; i < len; i++) {
            jl_value_t *prior_ti = jl_svecref(jl_array_ptr_ref(closure->t, i), 0);
            // in issue #13007 we incorrectly set skip=1 here, due to
            // Type{_<:T} ∩ (UnionAll S Type{T{S}}) = Type{T{S}}
            // Instead we should have computed the intersection as (UnionAll S Type{T{S}}),
            // which is a bigger type that would not have been a subtype of the prior
            // match (prior_ti). We simulate that for now by checking jl_has_typevars.
            if (jl_is_leaf_type(prior_ti) && !jl_has_typevars(closure->match.ti) && !jl_has_typevars(prior_ti) &&
                jl_subtype(closure->match.ti, prior_ti, 0)) {
                skip = 1;
                break;
            }
        }
        // don't analyze slots declared with ANY
        // TODO
        /*
        l = jl_nparams(ml->sig);
        size_t m = jl_nparams(ti);
        for(i=0; i < l && i < m; i++) {
            if (jl_tparam(ml->sig, i) == jl_ANY_flag)
                jl_tupleset(ti, i, jl_any_type);
        }
        */
    }
    if (!skip) {
        /*
          Check whether all static parameters matched. If not, then we
          have an argument type like Vector{T{Int,_}}, and a signature like
          f{A,B}(::Vector{T{A,B}}). If "_" turns out to be a non-typevar
          at runtime then this method matches, otherwise it doesn't. So we
          have to look for more matches. This caused issue #4731.
        */
        int matched_all_typevars = 1;
        size_t l = jl_svec_len(closure->match.env);
        for (i = 0; i < l; i++) {
            jl_value_t *tv;
            if (jl_is_typevar(ml->tvars))
                tv = (jl_value_t*)ml->tvars;
            else
                tv = jl_svecref(ml->tvars, i);
            if (jl_is_typevar(jl_svecref(closure->match.env, i)) &&
                // if tvar is at the top level it will definitely be matched.
                // see issue #5575
                !tvar_exists_at_top_level(tv, ml->sig, 1)) {
                matched_all_typevars = 0;
                break;
            }
        }
        int done = 0, return_this_match = 1;
        // (type ∩ ml->sig == type) ⇒ (type ⊆ ml->sig)
        // NOTE: jl_subtype check added in case the intersection is
        // over-approximated.
        if (matched_all_typevars && jl_types_equal(closure->match.ti, closure->match.type) &&
            jl_subtype(closure->match.type, (jl_value_t*)ml->sig, 0)) {
            done = 1; // terminate visiting method list
        }
        // here we have reached a definition that fully covers the arguments.
        // however, if there are ambiguities this method might not actually
        // match, so we shouldn't add it to the results.
        if (meth->ambig != jl_nothing && (!closure->include_ambiguous || done)) {
            jl_svec_t *env = NULL;
            JL_GC_PUSH1(&env);
            for (size_t j = 0; j < jl_array_len(meth->ambig); j++) {
                jl_method_t *mambig = (jl_method_t*)jl_array_ptr_ref(meth->ambig, j);
                env = jl_emptysvec;
                jl_value_t *mti = jl_type_intersection_matching((jl_value_t*)closure->match.type,
                                                                (jl_value_t*)mambig->sig,
                                                                &env, mambig->tvars);
                if (mti != (jl_value_t*)jl_bottom_type) {
                    if (closure->include_ambiguous) {
                        assert(done);
                        int k;
                        for(k=0; k < len; k++) {
                            if ((jl_value_t*)mambig == jl_svecref(jl_array_ptr_ref(closure->t, k), 2))
                                break;
                        }
                        if (k >= len) {
                            if (len == 0) {
                                closure->t = (jl_value_t*)jl_alloc_vec_any(0);
                            }
                            jl_array_ptr_1d_push((jl_array_t*)closure->t,
                                            (jl_value_t*)jl_svec(3, mti, env, mambig));
                            len++;
                        }
                    }
                    else {
                        // the current method doesn't match if there is an intersection with an
                        // ambiguous method that covers our intersection with this one.
                        jl_value_t *ambi = jl_type_intersection_matching((jl_value_t*)ml->sig,
                                                                         (jl_value_t*)mambig->sig,
                                                                         &env, mambig->tvars);
                        if (jl_subtype(closure->match.ti, ambi, 0)) {
                            return_this_match = 0;
                            break;
                        }
                    }
                }
            }
            JL_GC_POP();
        }
        if (return_this_match) {
            if (closure->lim >= 0 && len >= closure->lim) {
                closure->t = (jl_value_t*)jl_false;
                return 0; // terminate search
            }
            closure->matc = jl_svec(3, closure->match.ti, closure->match.env, meth);
            if (len == 0) {
                closure->t = (jl_value_t*)jl_alloc_vec_any(1);
                jl_array_ptr_set(closure->t, 0, (jl_value_t*)closure->matc);
            }
            else {
                jl_array_ptr_1d_push((jl_array_t*)closure->t, (jl_value_t*)closure->matc);
            }
        }
        if (done) return 0;
    }
    return 1;
}

// This is the collect form of calling jl_typemap_intersection_visitor
// with optimizations to skip fully shadowed methods.
//
// Returns a match as an array of svec(argtypes, static_params, Method).
// See below for the meaning of lim.
static jl_value_t *ml_matches(union jl_typemap_t defs, int offs,
                              jl_tupletype_t *type, int lim, int include_ambiguous)
{
    size_t l = jl_svec_len(type->parameters);
    jl_value_t *va = NULL;
    if (l > 0) {
        va = jl_tparam(type, l - 1);
        if (jl_is_vararg_type(va))
            va = jl_tparam0(va);
        else
            va = NULL;
    }
    struct ml_matches_env env;
    env.match.fptr = ml_matches_visitor;
    env.match.type = (jl_value_t*)type;
    env.match.va = va;
    env.match.ti = NULL;
    env.match.env = jl_emptysvec;
    env.t = jl_an_empty_vec_any;
    env.matc = NULL;
    env.lim = lim;
    env.include_ambiguous = include_ambiguous;
    JL_GC_PUSH4(&env.t, &env.matc, &env.match.env, &env.match.ti);
    jl_typemap_intersection_visitor(defs, offs, &env.match);
    JL_GC_POP();
    return env.t;
}

// return a Vector{Any} of svecs, each describing a method match:
// Any[svec(tt, spvals, m), ...]
// tt is the intersection of the type argument and the method signature,
// spvals is any matched static parameter values, m is the Method,
//
// lim is the max # of methods to return. if there are more, returns jl_false.
// -1 for no limit.
JL_DLLEXPORT jl_value_t *jl_matching_methods(jl_tupletype_t *types, int lim, int include_ambiguous)
{
    assert(jl_nparams(types) > 0);
    if (jl_tparam0(types) == jl_bottom_type)
        return (jl_value_t*)jl_alloc_vec_any(0);
    assert(jl_is_datatype(jl_tparam0(types)));
    jl_methtable_t *mt = ((jl_datatype_t*)jl_tparam0(types))->name->mt;
    if (mt == NULL)
        return (jl_value_t*)jl_alloc_vec_any(0);
    return ml_matches(mt->defs, 0, types, lim, include_ambiguous);
}

static jl_mutex_t typeinf_lock;

JL_DLLEXPORT void jl_typeinf_begin(void)
{
    JL_LOCK(&typeinf_lock);
}

JL_DLLEXPORT void jl_typeinf_end(void)
{
    JL_UNLOCK(&typeinf_lock);
}

#ifdef __cplusplus
}
#endif
