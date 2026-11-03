#include "ue/script_dump.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>

#include "core/log.h"
#include "core/memory.h"
#include "ue/names.h"
#include "ue/reflection.h"

namespace holevr::ue::script {
namespace {

// EExprToken values (UE 5.5 Engine/Source/Runtime/CoreUObject/Public/UObject/Script.h).
enum Op : uint8_t {
    EX_LocalVariable = 0x00,
    EX_InstanceVariable = 0x01,
    EX_DefaultVariable = 0x02,
    EX_Return = 0x04,
    EX_Jump = 0x06,
    EX_JumpIfNot = 0x07,
    EX_Assert = 0x09,
    EX_Nothing = 0x0B,
    EX_NothingInt32 = 0x0C,
    EX_Let = 0x0F,
    EX_BitFieldConst = 0x11,
    EX_ClassContext = 0x12,
    EX_MetaCast = 0x13,
    EX_LetBool = 0x14,
    EX_EndParmValue = 0x15,
    EX_EndFunctionParms = 0x16,
    EX_Self = 0x17,
    EX_Skip = 0x18,
    EX_Context = 0x19,
    EX_Context_FailSilent = 0x1A,
    EX_VirtualFunction = 0x1B,
    EX_FinalFunction = 0x1C,
    EX_IntConst = 0x1D,
    EX_FloatConst = 0x1E,
    EX_StringConst = 0x1F,
    EX_ObjectConst = 0x20,
    EX_NameConst = 0x21,
    EX_RotationConst = 0x22,
    EX_VectorConst = 0x23,
    EX_ByteConst = 0x24,
    EX_IntZero = 0x25,
    EX_IntOne = 0x26,
    EX_True = 0x27,
    EX_False = 0x28,
    EX_TextConst = 0x29,
    EX_NoObject = 0x2A,
    EX_TransformConst = 0x2B,
    EX_IntConstByte = 0x2C,
    EX_NoInterface = 0x2D,
    EX_DynamicCast = 0x2E,
    EX_StructConst = 0x2F,
    EX_EndStructConst = 0x30,
    EX_SetArray = 0x31,
    EX_EndArray = 0x32,
    EX_PropertyConst = 0x33,
    EX_UnicodeStringConst = 0x34,
    EX_Int64Const = 0x35,
    EX_UInt64Const = 0x36,
    EX_DoubleConst = 0x37,
    EX_Cast = 0x38,
    EX_SetSet = 0x39,
    EX_EndSet = 0x3A,
    EX_SetMap = 0x3B,
    EX_EndMap = 0x3C,
    EX_SetConst = 0x3D,
    EX_EndSetConst = 0x3E,
    EX_MapConst = 0x3F,
    EX_EndMapConst = 0x40,
    EX_Vector3fConst = 0x41,
    EX_StructMemberContext = 0x42,
    EX_LetMulticastDelegate = 0x43,
    EX_LetDelegate = 0x44,
    EX_LocalVirtualFunction = 0x45,
    EX_LocalFinalFunction = 0x46,
    EX_LocalOutVariable = 0x48,
    EX_DeprecatedOp4A = 0x4A,
    EX_InstanceDelegate = 0x4B,
    EX_PushExecutionFlow = 0x4C,
    EX_PopExecutionFlow = 0x4D,
    EX_ComputedJump = 0x4E,
    EX_PopExecutionFlowIfNot = 0x4F,
    EX_Breakpoint = 0x50,
    EX_InterfaceContext = 0x51,
    EX_ObjToInterfaceCast = 0x52,
    EX_EndOfScript = 0x53,
    EX_CrossInterfaceCast = 0x54,
    EX_InterfaceToObjCast = 0x55,
    EX_WireTracepoint = 0x5A,
    EX_SkipOffsetConst = 0x5B,
    EX_AddMulticastDelegate = 0x5C,
    EX_ClearMulticastDelegate = 0x5D,
    EX_Tracepoint = 0x5E,
    EX_LetObj = 0x5F,
    EX_LetWeakObjPtr = 0x60,
    EX_BindDelegate = 0x61,
    EX_RemoveMulticastDelegate = 0x62,
    EX_CallMulticastDelegate = 0x63,
    EX_LetValueOnPersistentFrame = 0x64,
    EX_ArrayConst = 0x65,
    EX_EndArrayConst = 0x66,
    EX_SoftObjectConst = 0x67,
    EX_CallMath = 0x68,
    EX_SwitchValue = 0x69,
    EX_InstrumentationEvent = 0x6A,
    EX_ArrayGetByRef = 0x6B,
    EX_ClassSparseDataVariable = 0x6C,
    EX_FieldPathConst = 0x6D,
};

// In-memory FScriptName: ComparisonIndex, DisplayIndex, Number.
constexpr int kScriptNameSize = 12;

std::string Fmt(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
std::string Fmt(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

class Decoder {
public:
    Decoder(const uint8_t* code, int32_t size) : code_(code), size_(size) {}

    std::string Run() {
        std::string out;
        while (ok_ && pos_ < size_) {
            const int32_t at = pos_;
            if (code_[pos_] == EX_EndOfScript) {
                out += Fmt("  0x%04x: end\n", at);
                ++pos_;
                continue;
            }
            std::string s = Expr(0);
            if (ok_ || pos_ > at) out += Fmt("  0x%04x: ", at) + s + "\n";
        }
        if (!ok_) {
            out += Fmt("  !! stopped at 0x%04x: %s; rest as hex:\n   ", bad_at_, why_.c_str());
            for (int32_t i = bad_at_, n = 0; i < size_ && n < 512; ++i, ++n) {
                out += Fmt(" %02x", code_[i]);
                if (n % 32 == 31) out += "\n   ";
            }
            out += "\n";
        }
        return out;
    }

private:
    bool Need(int32_t n) {
        if (pos_ + n <= size_) return true;
        Fail("ran past the end");
        return false;
    }
    void Fail(const std::string& why) {
        if (!ok_) return;
        ok_ = false;
        bad_at_ = pos_;
        why_ = why;
    }
    template <typename T>
    T Get() {
        T v{};
        if (!Need(sizeof(T))) return v;
        std::memcpy(&v, code_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    uintptr_t Ptr() { return Get<uintptr_t>(); }

    std::string ObjName(uintptr_t p) {
        if (!p) return "null";
        if (!mem::LooksLikePointer(p) || !mem::IsReadable(p, 0x28)) return Fmt("<obj %llx>", (unsigned long long)p);
        std::string n = GetName(p);
        return n.empty() ? Fmt("<obj %llx>", (unsigned long long)p) : n;
    }
    std::string FuncName(uintptr_t p) {
        if (!p) return "null";
        std::string n = ObjName(p);
        UObject outer = GetOuter(p);
        if (outer) {
            std::string o = GetName(outer);
            if (!o.empty()) return o + "::" + n;
        }
        return n;
    }
    std::string Prop(uintptr_t p) {
        if (!p) return "null";
        if (!mem::LooksLikePointer(p) || !mem::IsReadable(p, 0x30)) return Fmt("<prop %llx>", (unsigned long long)p);
        std::string n = reflection::FieldName(p);
        return n.empty() ? Fmt("<prop %llx>", (unsigned long long)p) : n;
    }
    std::string ScriptName() {
        if (!Need(kScriptNameSize)) return "?";
        uint32_t idx, display, number;
        std::memcpy(&idx, code_ + pos_, 4);
        std::memcpy(&display, code_ + pos_ + 4, 4);
        std::memcpy(&number, code_ + pos_ + 8, 4);
        pos_ += kScriptNameSize;
        FName n;
        n.comparison_index = idx;
        n.number = number;
        std::string s = names::ToString(n);
        return s.empty() ? Fmt("<name %u>", idx) : s;
    }
    std::string AnsiString() {
        std::string s;
        while (ok_ && Need(1) && code_[pos_] != 0) s.push_back(static_cast<char>(code_[pos_++]));
        if (ok_) ++pos_;
        return s;
    }
    std::string WideString() {
        std::string s;
        while (ok_ && Need(2)) {
            uint16_t c;
            std::memcpy(&c, code_ + pos_, 2);
            pos_ += 2;
            if (!c) break;
            s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        }
        return s;
    }
    // Expressions until `end` (consumed); comma separated.
    std::string ListUntil(uint8_t end, int depth) {
        std::string s;
        for (int n = 0; ok_ && n < 4096; ++n) {
            if (!Need(1)) break;
            if (code_[pos_] == end) {
                ++pos_;
                return s;
            }
            if (!s.empty()) s += ", ";
            s += Expr(depth + 1);
        }
        return s;
    }
    std::string Call(const std::string& name, int depth) {
        return name + "(" + ListUntil(EX_EndFunctionParms, depth) + ")";
    }

    std::string Expr(int depth) {
        if (!ok_) return "?";
        if (depth > 200) {
            Fail("nested too deep");
            return "?";
        }
        if (!Need(1)) return "?";
        const int32_t at = pos_;
        const uint8_t op = code_[pos_++];
        switch (op) {
            case EX_LocalVariable: return Prop(Ptr());
            case EX_InstanceVariable: return "this." + Prop(Ptr());
            case EX_DefaultVariable: return "default." + Prop(Ptr());
            case EX_LocalOutVariable: return "out:" + Prop(Ptr());
            case EX_ClassSparseDataVariable: return "sparse." + Prop(Ptr());
            case EX_Return: return "return " + Expr(depth + 1);
            case EX_Jump: return Fmt("goto 0x%04x", Get<uint32_t>());
            case EX_JumpIfNot: {
                uint32_t to = Get<uint32_t>();
                return "if not (" + Expr(depth + 1) + Fmt(") goto 0x%04x", to);
            }
            case EX_Assert: {
                Get<uint16_t>();
                Get<uint8_t>();
                return "assert(" + Expr(depth + 1) + ")";
            }
            case EX_Nothing: return "nop";
            case EX_NothingInt32: return Fmt("nop(%d)", Get<int32_t>());
            case EX_EndOfScript: return "end";
            case EX_Let: {
                Ptr();
                std::string l = Expr(depth + 1);
                return l + " = " + Expr(depth + 1);
            }
            case EX_LetBool:
            case EX_LetObj:
            case EX_LetWeakObjPtr:
            case EX_LetDelegate:
            case EX_LetMulticastDelegate: {
                std::string l = Expr(depth + 1);
                return l + " = " + Expr(depth + 1);
            }
            case EX_LetValueOnPersistentFrame: {
                std::string p = Prop(Ptr());
                return "frame." + p + " = " + Expr(depth + 1);
            }
            case EX_BitFieldConst: {
                std::string p = Prop(Ptr());
                return Fmt("bitfield(%s, %u)", p.c_str(), Get<uint8_t>());
            }
            case EX_ClassContext:
            case EX_Context:
            case EX_Context_FailSilent: {
                std::string obj = Expr(depth + 1);
                Get<uint32_t>();  // skip size
                Ptr();            // r-value property
                return obj + (op == EX_ClassContext ? "::" : ".") + Expr(depth + 1);
            }
            case EX_MetaCast: {
                std::string c = ObjName(Ptr());
                return "metacast<" + c + ">(" + Expr(depth + 1) + ")";
            }
            case EX_DynamicCast: {
                std::string c = ObjName(Ptr());
                return "cast<" + c + ">(" + Expr(depth + 1) + ")";
            }
            case EX_ObjToInterfaceCast:
            case EX_CrossInterfaceCast:
            case EX_InterfaceToObjCast: {
                std::string c = ObjName(Ptr());
                return "icast<" + c + ">(" + Expr(depth + 1) + ")";
            }
            case EX_Cast: {
                uint8_t t = Get<uint8_t>();
                return Fmt("conv%u(", t) + Expr(depth + 1) + ")";
            }
            case EX_EndParmValue: return "endparm";
            case EX_EndFunctionParms: return "endparms";
            case EX_Self: return "self";
            case EX_Skip: {
                Get<uint32_t>();
                return Expr(depth + 1);
            }
            case EX_VirtualFunction: return Call(ScriptName(), depth);
            case EX_LocalVirtualFunction: return Call("local:" + ScriptName(), depth);
            case EX_FinalFunction:
            case EX_CallMath: return Call(FuncName(Ptr()), depth);
            case EX_LocalFinalFunction: return Call("local:" + FuncName(Ptr()), depth);
            case EX_CallMulticastDelegate: {
                std::string sig = FuncName(Ptr());
                std::string d = Expr(depth + 1);
                return "broadcast " + d + "(" + ListUntil(EX_EndFunctionParms, depth) + ") [" + sig + "]";
            }
            case EX_IntConst: return Fmt("%d", Get<int32_t>());
            case EX_FloatConst: return Fmt("%gf", Get<float>());
            case EX_DoubleConst: return Fmt("%g", Get<double>());
            case EX_Int64Const: return Fmt("%lld", (long long)Get<int64_t>());
            case EX_UInt64Const: return Fmt("%llu", (unsigned long long)Get<uint64_t>());
            case EX_StringConst: return "\"" + AnsiString() + "\"";
            case EX_UnicodeStringConst: return "\"" + WideString() + "\"";
            case EX_ObjectConst: return "obj:" + ObjName(Ptr());
            case EX_NameConst: return "name:" + ScriptName();
            case EX_RotationConst: {
                double p = Get<double>(), y = Get<double>(), r = Get<double>();
                return Fmt("rot(%g, %g, %g)", p, y, r);
            }
            case EX_VectorConst: {
                double x = Get<double>(), y = Get<double>(), z = Get<double>();
                return Fmt("vec(%g, %g, %g)", x, y, z);
            }
            case EX_Vector3fConst: {
                float x = Get<float>(), y = Get<float>(), z = Get<float>();
                return Fmt("vec3f(%g, %g, %g)", x, y, z);
            }
            case EX_TransformConst: {
                double v[10];
                for (double& d : v) d = Get<double>();
                return Fmt("xform(q %g %g %g %g, t %g %g %g, s %g %g %g)", v[0], v[1], v[2], v[3], v[4], v[5], v[6],
                           v[7], v[8], v[9]);
            }
            case EX_ByteConst:
            case EX_IntConstByte: return Fmt("%u", Get<uint8_t>());
            case EX_IntZero: return "0";
            case EX_IntOne: return "1";
            case EX_True: return "true";
            case EX_False: return "false";
            case EX_NoObject: return "none";
            case EX_NoInterface: return "no_interface";
            case EX_TextConst: {
                uint8_t t = Get<uint8_t>();
                switch (t) {
                    case 0: return "text\"\"";
                    case 1: {
                        std::string a = Expr(depth + 1);
                        Expr(depth + 1);  // key
                        Expr(depth + 1);  // namespace
                        return "text" + a;
                    }
                    case 2:
                    case 3: return "text" + Expr(depth + 1);
                    case 4: {
                        Ptr();
                        std::string a = Expr(depth + 1);
                        return "strtable(" + a + ", " + Expr(depth + 1) + ")";
                    }
                    default: Fail(Fmt("text literal type %u", t)); return "?";
                }
            }
            case EX_StructConst: {
                std::string s = ObjName(Ptr());
                Get<int32_t>();
                return s + "{" + ListUntil(EX_EndStructConst, depth) + "}";
            }
            case EX_SetArray: {
                std::string a = Expr(depth + 1);
                return a + " = [" + ListUntil(EX_EndArray, depth) + "]";
            }
            case EX_SetSet: {
                std::string a = Expr(depth + 1);
                Get<int32_t>();
                return a + " = set{" + ListUntil(EX_EndSet, depth) + "}";
            }
            case EX_SetMap: {
                std::string a = Expr(depth + 1);
                Get<int32_t>();
                return a + " = map{" + ListUntil(EX_EndMap, depth) + "}";
            }
            case EX_ArrayConst: {
                Ptr();
                Get<int32_t>();
                return "[" + ListUntil(EX_EndArrayConst, depth) + "]";
            }
            case EX_SetConst: {
                Ptr();
                Get<int32_t>();
                return "set{" + ListUntil(EX_EndSetConst, depth) + "}";
            }
            case EX_MapConst: {
                Ptr();
                Ptr();
                Get<int32_t>();
                return "map{" + ListUntil(EX_EndMapConst, depth) + "}";
            }
            case EX_PropertyConst: return "prop:" + Prop(Ptr());
            case EX_StructMemberContext: {
                std::string m = Prop(Ptr());
                return Expr(depth + 1) + "." + m;
            }
            case EX_InstanceDelegate: return "delegate:" + ScriptName();
            case EX_BindDelegate: {
                std::string fn = ScriptName();
                std::string d = Expr(depth + 1);
                return "bind " + d + " to " + Expr(depth + 1) + "." + fn;
            }
            case EX_AddMulticastDelegate:
            case EX_RemoveMulticastDelegate: {
                std::string d = Expr(depth + 1);
                return d + (op == EX_AddMulticastDelegate ? " += " : " -= ") + Expr(depth + 1);
            }
            case EX_ClearMulticastDelegate: return "clear " + Expr(depth + 1);
            case EX_PushExecutionFlow: return Fmt("push_flow 0x%04x", Get<uint32_t>());
            case EX_PopExecutionFlow: return "pop_flow";
            case EX_ComputedJump: return "goto computed(" + Expr(depth + 1) + ")";
            case EX_PopExecutionFlowIfNot: return "pop_flow if not (" + Expr(depth + 1) + ")";
            case EX_Breakpoint:
            case EX_Tracepoint:
            case EX_WireTracepoint:
            case EX_DeprecatedOp4A: return "trace";
            case EX_InterfaceContext: return "iface(" + Expr(depth + 1) + ")";
            case EX_SkipOffsetConst: return Fmt("skip 0x%04x", Get<uint32_t>());
            case EX_SoftObjectConst: return "soft(" + Expr(depth + 1) + ")";
            case EX_FieldPathConst: return "fieldpath(" + Expr(depth + 1) + ")";
            case EX_ArrayGetByRef: {
                std::string a = Expr(depth + 1);
                return a + "[" + Expr(depth + 1) + "]";
            }
            case EX_SwitchValue: {
                uint16_t n = Get<uint16_t>();
                Get<uint32_t>();  // end offset
                std::string s = "switch(" + Expr(depth + 1) + "){";
                for (uint16_t i = 0; ok_ && i < n; ++i) {
                    std::string v = Expr(depth + 1);
                    Get<uint32_t>();  // next case offset
                    s += v + ": " + Expr(depth + 1) + "; ";
                }
                return s + "default: " + Expr(depth + 1) + "}";
            }
            case EX_InstrumentationEvent: {
                uint8_t t = Get<uint8_t>();
                if (t == 4) ScriptName();  // EScriptInstrumentation::InlineEvent
                return Fmt("instrument(%u)", t);
            }
            default:
                pos_ = at;
                Fail(Fmt("unknown opcode 0x%02x", op));
                return "?";
        }
    }

    const uint8_t* code_;
    int32_t size_;
    int32_t pos_ = 0;
    bool ok_ = true;
    int32_t bad_at_ = 0;
    std::string why_;
};

// Returns false when the listing stopped at a byte the decoder doesn't know.
bool WriteFunction(FILE* f, UObject fn) {
    std::string params;
    for (const auto& p : reflection::GetProperties(fn)) {
        if (!(p.flags & reflection::CPF_Parm)) continue;
        if (!params.empty()) params += ", ";
        if (p.flags & reflection::CPF_ReturnParm) params += "return ";
        else if (p.flags & reflection::CPF_OutParm) params += "out ";
        params += p.type + (p.inner_type.empty() ? "" : "<" + p.inner_type + ">") + " " + p.name;
    }
    fprintf(f, "fn %s(%s)\n", GetName(fn).c_str(), params.c_str());
    std::string body = Disassemble(fn);
    if (body.empty()) fputs("  (native or empty)\n", f);
    else fputs(body.c_str(), f);
    fputc('\n', f);
    return body.find("!! stopped") == std::string::npos;
}

}  // namespace

std::string Disassemble(UObject function) {
    uintptr_t data = 0;
    int32_t size = 0;
    if (!reflection::GetScript(function, &data, &size)) return {};
    std::string copy(static_cast<size_t>(size), '\0');
    if (!mem::SafeCopy(copy.data(), reinterpret_cast<const void*>(data), copy.size())) return {};
    Decoder d(reinterpret_cast<const uint8_t*>(copy.data()), size);
    return Fmt("  (%d bytes)\n", size) + d.Run();
}

int DumpClasses(const std::wstring& dump_dir, const std::vector<std::string>& filters) {
    if (!reflection::Ready() || filters.empty()) return 0;
    std::wstring dir = dump_dir + L"\\script";
    CreateDirectoryW(dir.c_str(), nullptr);
    static const std::set<std::string> kBlueprintClasses = {"BlueprintGeneratedClass", "WidgetBlueprintGeneratedClass",
                                                           "AnimBlueprintGeneratedClass"};
    std::vector<UObject> classes;
    objects::ForEach([&](int, UObject o) {
        if (!kBlueprintClasses.count(GetName(GetClass(o)))) return;
        const std::string path = GetPathName(o);
        for (const auto& flt : filters)
            if (!flt.empty() && path.find(flt) != std::string::npos) {
                classes.push_back(o);
                return;
            }
    });
    int written = 0, functions = 0, failed = 0;
    for (UObject c : classes) {
        std::string name = GetName(c);
        std::wstring file = dir + L"\\" + std::wstring(name.begin(), name.end()) + L".txt";
        FILE* f = _wfopen(file.c_str(), L"w");
        if (!f) continue;
        UObject super = reflection::GetSuper(c);
        fprintf(f, "%s : %s\n\n", GetFullName(c).c_str(), super ? GetName(super).c_str() : "-");
        for (UObject fn : reflection::GetFunctions(c)) {
            if (!WriteFunction(f, fn)) ++failed;
            ++functions;
        }
        fclose(f);
        ++written;
    }
    log::Info("script dump: %d classes, %d functions listed in dump/script (%d listings stopped at an unknown byte)",
              written, functions, failed);
    return written;
}

}  // namespace holevr::ue::script
