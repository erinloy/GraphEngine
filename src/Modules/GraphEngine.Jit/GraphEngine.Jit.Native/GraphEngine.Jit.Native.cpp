#include "TrinityCommon.h"
#include "asmjit.h"
#include "TypeSystem.h"
#include "JitRoutines.h"
#include <vector>
#include <map>

using namespace asmjit;

static JitRuntime s_runtime;
static std::map<VerbCode, JitProc> s_jitprocs{
    {VerbCode::VC_BGet       , BGet},
    {VerbCode::VC_BSet       , BSet},
    {VerbCode::VC_SGet       , SGet},
    {VerbCode::VC_SSet       , SSet},
    {VerbCode::VC_GSGet      , GSGet},
    {VerbCode::VC_GSSet      , GSSet},
    {VerbCode::VC_LGet       , LGet},
    {VerbCode::VC_LSet       , LSet},
    {VerbCode::VC_LInlineGet , LInlineGet},
    {VerbCode::VC_LInlineSet , LInlineSet},
    {VerbCode::VC_LContains  , LContains},
    {VerbCode::VC_LCount     , LCount},
};

static std::map<TypeCode, TypeId::Id> s_atom_typemap{
    {TypeCode::TC_U8         , TypeId::kU8 }, 
    {TypeCode::TC_U16        , TypeId::kU16 }, 
    {TypeCode::TC_U32        , TypeId::kU32 }, 
    {TypeCode::TC_U64        , TypeId::kU64 },
    {TypeCode::TC_I8         , TypeId::kI8 }, 
    {TypeCode::TC_I16        , TypeId::kI16 }, 
    {TypeCode::TC_I32        , TypeId::kI32 }, 
    {TypeCode::TC_I64        , TypeId::kI64 },
    {TypeCode::TC_F32        , TypeId::kF32 }, 
    {TypeCode::TC_F64        , TypeId::kF64 },
    {TypeCode::TC_BOOL       , TypeId::kU8 },
    {TypeCode::TC_CHAR       , TypeId::kU16 }, 
};

static TypeId::Id _get_typeid(IN TypeDescriptor* const type)
{
    auto c = static_cast<TypeCode>(type->get_TypeCode());
    auto i = s_atom_typemap.find(c);
    return i != s_atom_typemap.cend() ? i->second : TypeId::kUIntPtr;
}

static MemberDescriptor* _find_member(TypeDescriptor* type, char* name)
{
    auto members = type->get_Members();
    auto i = std::find_if(members->begin(), members->end(), [=](auto m) { return !strcmp(m->Name, name); });
    auto ret = *i;
    delete members;
    return ret;
}

static TypeId::Id _get_retid(IN FunctionDescriptor* fdesc)
{
    if (fdesc->Verbs->is_setter()) return TypeId::kVoid;

    TypeDescriptor* type = &fdesc->Type;
    TypeId::Id ret;
    std::vector<TypeDescriptor*> *vec = nullptr;

    for (Verb *v = fdesc->Verbs, *pend = fdesc->Verbs + fdesc->NrVerbs; v != pend; ++v) 
    {
        switch (v->Code) {
        case VerbCode::VC_BGet:
            return _get_typeid(type);
        case VerbCode::VC_GSGet:
            type = v->Data.GenericTypeArgument;
            ret = TypeId::kUIntPtr;
            break;
        case VerbCode::VC_SGet:
            type = &_find_member(type, v->Data.MemberName)->Type;
            ret = _get_typeid(type);
            break;
        case VerbCode::VC_LGet:
        case VerbCode::VC_LInlineGet:
            vec = type->get_ElementType();
            type = vec->at(0);
            ret = _get_typeid(type);
            delete vec;
        case VerbCode::VC_LContains:
            return TypeId::kU8;
        case VerbCode::VC_LCount:
            return TypeId::kI32;
        default:
            throw;
        }
    }

    return ret;
}

static void _get_args(IN FunctionDescriptor* fdesc, OUT uint8_t* &pargs, OUT int32_t& nargs)
{
    TypeDescriptor* type = &fdesc->Type;
    std::vector<uint8_t> vec{ TypeId::kUIntPtr };
    std::vector<TypeDescriptor*> *tvec;

    // for all setters, lcontains, lcount and bget, we allow no further sub-verbs.

    for (Verb *v = fdesc->Verbs, *pend = fdesc->Verbs + fdesc->NrVerbs; v != pend; ++v) 
    {
        switch (v->Code) {

        case VerbCode::VC_BGet:
            goto out;
        case VerbCode::VC_BSet:
            vec.push_back(_get_typeid(type));
            goto out;

        case VerbCode::VC_LGet:
            vec.push_back(TypeId::kI32);
            /* FALLTHROUGH */
        case VerbCode::VC_LInlineGet:  // no indexer
            tvec = type->get_ElementType();
            type = tvec->at(0);
            delete tvec;
            break;

        case VerbCode::VC_LSet:
            vec.push_back(TypeId::kI32);
            /* FALLTHROUGH */
        case VerbCode::VC_LInlineSet:  // no indexer
            tvec = type->get_ElementType();
            type = tvec->at(0);
            vec.push_back(_get_typeid(type));
            delete tvec;
            goto out;

        case VerbCode::VC_LContains:
            tvec = type->get_ElementType();
            type = tvec->at(0);
            vec.push_back(_get_typeid(type));
            delete tvec;
            goto out;
        case VerbCode::VC_LCount:
            goto out;

        case VerbCode::VC_GSGet:
            vec.push_back(TypeId::kUIntPtr);  // char* member_name
            type = v->Data.GenericTypeArgument;
            break;
        case VerbCode::VC_SGet:
            vec.push_back(TypeId::kUIntPtr);  // char* member_name
            type = v->Data.GenericTypeArgument;
            vec.push_back(_get_typeid(type)); // then push back the set value
            goto out;
        default:
            throw;
        }
    }

    out:

    nargs = vec.size();
    pargs = (uint8_t*)malloc(nargs * sizeof(uint8_t));
    std::copy(vec.begin(), vec.end(), pargs);
}

JitRoutine(Dispatch) { s_jitprocs[v->Code](cc, ctx, type, v); }

DLL_EXPORT void* CompileFunctionToNative(FunctionDescriptor* fdesc)
{
    CodeHolder      code;
    CodeInfo        ci  = s_runtime.getCodeInfo();
    CallConv::Id    ccId = CallConv::kIdHost;
    void*           ret = nullptr;
    FuncSignature   fsig;
    TypeId::Id      retId;
    uint8_t*        pargs;
    int32_t         nargs;
    CCFunc*         func;

    if (code.init(ci)) throw;
    X86Compiler cc(&code);

    retId = _get_typeid(&fdesc->Type);
    _get_args(fdesc, pargs, nargs);

    fsig.init(ccId, retId, pargs, nargs);
    func = cc.addFunc(fsig);

    // 1st arg is always cellptr
    auto cellPtr = cc.newGpq("cellPtr");
    FuncCtx fctx(&cellPtr);

    Dispatch(cc, fctx, &fdesc->Type, fdesc->Verbs);

    if (cc.finalize()) throw;
    if (s_runtime.add(&ret, &code)) throw;

    fdesc->~FunctionDescriptor();
    free(fdesc);

    return ret;
}

JitRoutine(BGet)
{
    auto address = x86::ptr(*ctx.cellPtr);
    auto retreg = cc.newGpq();
    cc.mov(retreg, address);
    cc.ret(retreg);
}

JitRoutine(BSet)
{
    auto address = x86::ptr(*ctx.cellPtr);
    auto regarg = cc.newGpq();
    cc.setArg(ctx.newArg(), regarg);
    cc.mov(address, regarg);
    cc.ret();
}

JitRoutine(SGet) 
{
    auto member = _find_member(type, v->Data.MemberName);
    int member_offset = 0; //TODO calculate offset of member

    cc.add(*ctx.cellPtr, member_offset);

    Dispatch(cc, ctx, &member->Type, v + 1);
}

JitRoutine(SSet)
{
    auto member = _find_member(type, v->Data.MemberName);
    int member_offset = 0; //TODO calculate offset of member

    cc.add(*ctx.cellPtr, member_offset);
    cc.ret();
    //TODO assign
}

JitRoutine(GSGet)
{

}
JitRoutine(GSSet) 
{

}

JitRoutine(LGet)
{

}
JitRoutine(LSet)
{

}

JitRoutine(LInlineGet)
{

}
JitRoutine(LInlineSet) 
{

}

JitRoutine(LContains)
{

}
JitRoutine(LCount) 
{

}
