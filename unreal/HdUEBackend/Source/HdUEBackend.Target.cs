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
        ExtraModuleNames.AddRange(new[] { "HdUEBackend", "UELibrary" });
        bCompileICU = true;
        // NaniteBuilder (a Developer module) builds Nanite data for the meshes hdUE creates at runtime.
        bBuildDeveloperTools = true;
    }
}

