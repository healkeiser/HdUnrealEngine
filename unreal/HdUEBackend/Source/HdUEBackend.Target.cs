using UnrealBuildTool;
public class HdUEBackendTarget : TargetRules
{
    public HdUEBackendTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        LinkType = TargetLinkType.Monolithic;
        BuildEnvironment = TargetBuildEnvironment.Unique;
        bShouldCompileAsDLL = true;
        GlobalDefinitions.Add("UE_LIBRARY_ENABLED=1");
        // UE runs on a thread of its host (Houdini's, husk's), not the thread that ran static initialization: without
        // this, FTaskTagScope ensures on every start (as on Apple and Android, where Core.Build.cs sets it).
        GlobalDefinitions.Add("IS_RUNNING_GAMETHREAD_ON_EXTERNAL_THREAD=1");
        // Call-stack tracing (Insights) walks the process's module list without the loader lock at start-up
        // (FModuleTrace::Initialize, WindowsModuleDiagnostics.cpp). In a host that loads and unloads DLLs on other
        // threads meanwhile (Houdini starting up), it read an unloaded module and crashed. Off, as UE has it on every
        // platform but Windows (CallstackTrace.h).
        GlobalDefinitions.Add("UE_CALLSTACK_TRACE_ENABLED=0");
        ExtraModuleNames.AddRange(new[] { "HdUEBackend", "UELibrary" });
        bCompileICU = true;
        // NaniteBuilder (a Developer module) builds Nanite data for the meshes hdUE creates at runtime.
        bBuildDeveloperTools = true;
        // Packages ship the Test configuration, which compiles logging out by default (NO_LOGGING, Build.h): no log file,
        // and none of UE's warnings and errors in husk's or Houdini's console.
        bUseLoggingInShipping = true;
    }
}

