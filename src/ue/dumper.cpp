#include "ue/dumper.h"

#include <windows.h>

#include <cstdio>
#include <set>
#include <string>

#include "core/log.h"
#include "ue/objects.h"
#include "ue/reflection.h"

namespace holevr::ue::dumper {
namespace {

const std::set<std::string> kStructClasses = {
    "Class",
    "BlueprintGeneratedClass",
    "WidgetBlueprintGeneratedClass",
    "AnimBlueprintGeneratedClass",
    "ScriptStruct",
    "UserDefinedStruct",
};

// Engine modules every UE game ships. Anything outside these is likely the
// game's own code or a third-party plugin worth looking at.
bool IsEngineScriptPackage(const std::string& path) {
    static const char* kPrefixes[] = {"/Script/CoreUObject", "/Script/Engine", "/Script/UMG",
                                      "/Script/Slate", "/Script/AIModule", "/Script/Niagara",
                                      "/Script/Chaos", "/Script/Anim", "/Script/Control",
                                      "/Script/Movie", "/Script/Media", "/Script/Audio",
                                      "/Script/Enhanced", "/Script/Gameplay", "/Script/Online"};
    for (const char* p : kPrefixes)
        if (path.rfind(p, 0) == 0) return true;
    return false;
}

bool IsGameContent(const std::string& path) {
    if (path.rfind("/Game/", 0) == 0) return true;
    return path.rfind("/Script/", 0) == 0 && !IsEngineScriptPackage(path) &&
           path.find("HOLE") != std::string::npos;
}

std::string Hex(long long v) {
    char b[32];
    snprintf(b, sizeof(b), "0x%llx", v);
    return b;
}

void WriteStruct(FILE* f, UObject s) {
    using namespace reflection;
    UObject super = GetSuper(s);
    fprintf(f, "%s : %s  (size %s)\n", GetFullName(s).c_str(),
            super ? GetName(super).c_str() : "-", Hex(GetPropertiesSize(s)).c_str());
    for (const auto& p : GetProperties(s)) {
        fprintf(f, "    [%s] %s%s%s%s %s", Hex(p.offset).c_str(), p.type.c_str(),
                p.inner_type.empty() ? "" : "<", p.inner_type.c_str(),
                p.inner_type.empty() ? "" : ">", p.name.c_str());
        if (p.array_dim > 1) fprintf(f, "[%d]", p.array_dim);
        fprintf(f, "  (size %s)\n", Hex(p.element_size).c_str());
    }
    for (UObject fn : GetFunctions(s)) {
        std::string ret = "void", params;
        for (const auto& p : GetProperties(fn)) {
            std::string t = p.inner_type.empty() ? p.type : p.type + "<" + p.inner_type + ">";
            if (p.flags & CPF_ReturnParm) {
                ret = t;
                continue;
            }
            if (!params.empty()) params += ", ";
            if (p.flags & CPF_OutParm) params += "out ";
            params += t + " " + p.name;
        }
        fprintf(f, "    fn %s %s(%s)\n", ret.c_str(), GetName(fn).c_str(), params.c_str());
    }
    fputc('\n', f);
}

}  // namespace

void DumpAll(const std::wstring& mod_dir) {
    std::wstring dir = mod_dir + L"\\dump";
    CreateDirectoryW(dir.c_str(), nullptr);
    DWORD start = GetTickCount();

    FILE* objs = _wfopen((dir + L"\\objects.txt").c_str(), L"w");
    FILE* classes = _wfopen((dir + L"\\classes.txt").c_str(), L"w");
    FILE* game = _wfopen((dir + L"\\game_classes.txt").c_str(), L"w");
    if (!objs || !classes || !game) {
        log::Error("dump: could not open files in the dump folder");
        if (objs) fclose(objs);
        if (classes) fclose(classes);
        if (game) fclose(game);
        return;
    }
    int n_objects = 0, n_structs = 0, n_game = 0;
    objects::ForEach([&](int index, UObject o) {
        std::string class_name = GetName(GetClass(o));
        std::string path = GetPathName(o);
        fprintf(objs, "%7d  %s %s\n", index, class_name.c_str(), path.c_str());
        ++n_objects;
        if (!reflection::Ready() || !kStructClasses.count(class_name)) return;
        WriteStruct(classes, o);
        ++n_structs;
        if (IsGameContent(path)) {
            WriteStruct(game, o);
            ++n_game;
        }
    });
    fclose(objs);
    fclose(classes);
    fclose(game);
    log::Info("dump: %d objects, %d classes/structs (%d from the game itself) in %lu ms",
              n_objects, n_structs, n_game, GetTickCount() - start);
}

}  // namespace holevr::ue::dumper
